#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpTestExecution.h"

#include "OloEngine/Core/Environment.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Structured test execution: olo_tests_list and olo_tests_run (issue #1130,
// Epic H slice 1).
//
// WHERE THIS RUNS, and why that was the first decision. The commands execute
// OloEngine-Tests as a CHILD PROCESS from the automation handler thread. They
// never touch the editor's renderer, never MarshalRead onto the game thread,
// and hold no GL/Vulkan state — which is the issue's "must not require the
// editor's renderer for the CPU-only suites" constraint met structurally rather
// than by a guard someone can forget. A GPU suite still skips or runs on its
// own terms inside the child, exactly as it does from a shell.
//
// Results come from gtest's own JSON report, not from console text. The console
// IS captured, but only for two things a JSON file cannot give: live progress
// while the run is in flight (counting `[ RUN      ]` banners), and the tail of
// the log when the child died before writing a report at all.
//
// The correctness rule, stated once here and enforced in three places below: a
// run must never be silently partial. Zero matches is an error; a dead child is
// an error; a report that does not account for every selected case is an error
// naming the ones that went missing. See MCP/McpTestExecution.h.

namespace OloEngine::MCP
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace OloEngine::MCP::TestExecution;

        // The repo marker that identifies the source tree: the test catalogue
        // this command reads its classification from. Anchoring on the very file
        // we need means a root we "found" without one is not a root at all.
        constexpr const char* kCatalogueRelative = "OloEngine/tests/scripts/test_catalogue.json";
        constexpr const char* kTestRoot = "OloEngine/tests";

        // The child's working directory, relative to the repo root. This is what
        // ctest uses — OloEngine/tests/CMakeLists.txt passes
        // `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/OloEditor` to both
        // gtest_discover_tests calls, because "many tests call
        // Shader::Create("assets/shaders/...") which only works when CWD is
        // OloEditor". Matching it means a run from here and a run from CI are
        // the same run.
        //
        // Measured 2026-09-09: the engine also resolves those asset paths from
        // the repo root (FluidVisualEvidenceTest wrote its PNGs into the tracked
        // OloEditor/assets/tests/visual/ with CWD at the root, creating no stray
        // directory), so this is not the difference between working and broken
        // today. It is the difference between agreeing with CI and relying on a
        // fallback nobody promised.
        constexpr const char* kChildWorkingDirectory = "OloEditor";
        constexpr const char* kTestBinaryName =
#ifdef _WIN32
            "OloEngine-Tests.exe";
#else
            "OloEngine-Tests";
#endif

        // How far up from the working directory to look for the repo root. The
        // editor runs with cwd = OloEditor/, so one level suffices; the margin
        // covers a caller that launched it from deeper (a per-scene subdirectory).
        constexpr int kMaxRootSearchDepth = 8;

        // Poll cadence while a child runs: often enough that a cancellation is
        // acted on promptly, rarely enough that re-reading a growing console log
        // costs nothing measurable next to a test suite.
        constexpr auto kPollInterval = std::chrono::milliseconds(250);

        // Console tail returned when a child dies. Big enough for a stack dump
        // and the last few cases, small enough not to swamp a tool result.
        constexpr sizet kConsoleTailChars = 8000;

        // ---- filesystem helpers -------------------------------------------

        [[nodiscard]] std::string ReadWholeFile(const fs::path& path, bool& ok)
        {
            ok = false;
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
                return {};
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            ok = true;
            return buffer.str();
        }

        // Format a file's last-write time as an ISO-8601 UTC instant. Reported
        // next to every binary this command runs: an exit code and a green
        // summary look identical whether the binary under test is the one you
        // just built or last week's, so the answer says WHICH artefact produced
        // it and how old that artefact is.
        [[nodiscard]] std::string FileModifiedUtc(const fs::path& path)
        {
            std::error_code ec;
            const auto written = fs::last_write_time(path, ec);
            if (ec)
                return {};
            const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(written);
            const std::time_t epoch = std::chrono::system_clock::to_time_t(systemTime);
            std::tm utc{};
#ifdef _WIN32
            if (::gmtime_s(&utc, &epoch) != 0)
                return {};
#else
            if (::gmtime_r(&epoch, &utc) == nullptr)
                return {};
#endif
            std::ostringstream formatted;
            formatted << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
            return formatted.str();
        }

        // Walk up from the working directory looking for the test catalogue.
        [[nodiscard]] bool FindRepoRoot(fs::path& outRoot, std::string& outError)
        {
            std::error_code ec;
            fs::path cursor = fs::current_path(ec);
            if (ec)
            {
                outError = "Cannot determine the working directory: " + ec.message();
                return false;
            }
            std::string searched;
            for (int depth = 0; depth < kMaxRootSearchDepth; ++depth)
            {
                if (!searched.empty())
                    searched += ", ";
                searched += cursor.generic_string();
                if (fs::exists(cursor / kCatalogueRelative, ec))
                {
                    outRoot = cursor;
                    return true;
                }
                const fs::path parent = cursor.parent_path();
                if (parent.empty() || parent == cursor)
                    break;
                cursor = parent;
            }
            outError = std::string("Could not locate the OloEngine source tree: no ") + kCatalogueRelative +
                       " in any parent of the working directory (looked in " + searched + ").";
            return false;
        }

        // ---- the test binary ----------------------------------------------

        struct ResolvedBinary
        {
            fs::path Path;
            u64 SizeBytes = 0;
            std::string ModifiedUtc;
            std::string Origin; // how it was chosen: "argument", "environment", "search"
        };

        [[nodiscard]] Json BinaryJson(const ResolvedBinary& binary)
        {
            Json j;
            j["path"] = binary.Path.generic_string();
            j["sizeBytes"] = binary.SizeBytes;
            if (!binary.ModifiedUtc.empty())
                j["modifiedUtc"] = binary.ModifiedUtc;
            j["origin"] = binary.Origin;
            return j;
        }

        // The build trees CMakePresets.json defines, newest artefact wins. The
        // order here is only the order they are REPORTED in; selection is by
        // modification time, because "the one I just built" is what a caller
        // asking to run its tests means, and it is not always the default tree.
        [[nodiscard]] std::vector<fs::path> CandidateBinaries(const fs::path& repoRoot)
        {
            std::vector<fs::path> candidates;
            for (const char* tree : { "build-cached", "build", "build-clang" })
            {
                for (const char* config : { "Debug", "Release", "RelWithDebInfo", "" })
                {
                    fs::path candidate = repoRoot / tree / "OloEngine" / "tests";
                    if (*config != '\0')
                        candidate /= config;
                    candidates.push_back(candidate / kTestBinaryName);
                }
            }
            return candidates;
        }

        [[nodiscard]] bool DescribeBinary(const fs::path& path, ResolvedBinary& out, std::string& outError)
        {
            std::error_code ec;
            if (!fs::is_regular_file(path, ec))
            {
                outError = "Not a file: " + path.generic_string();
                return false;
            }
            out.Path = fs::weakly_canonical(path, ec);
            if (ec)
                out.Path = path;
            out.SizeBytes = static_cast<u64>(fs::file_size(out.Path, ec));
            if (ec)
                out.SizeBytes = 0;
            out.ModifiedUtc = FileModifiedUtc(out.Path);
            return true;
        }

        // Resolve which OloEngine-Tests to run. An explicit `binary` argument or
        // OLO_TESTS_BINARY wins and is NEVER silently replaced by a search hit:
        // a caller that named a binary and got a different one's results has
        // been lied to. Otherwise the newest build-tree artefact wins, and every
        // path that was considered is reported either way.
        [[nodiscard]] bool ResolveBinary(const Json& args, const fs::path& repoRoot, ResolvedBinary& out,
                                         Json& outSearched, std::string& outError)
        {
            outSearched = Json::array();

            const auto fromExplicitPath = [&](const std::string& raw, const char* origin) -> bool
            {
                fs::path path(raw);
                if (path.is_relative())
                    path = repoRoot / path;
                outSearched.push_back(path.generic_string());
                // The command exists to run THE TEST BINARY. Without this the
                // `binary` argument is a way to execute any executable on the
                // box through a command that is deliberately not consent-gated,
                // which is a much larger authority than "run the tests" — and
                // pointing it at anything else only ever produces the
                // "exited without writing a report" error anyway.
                if (path.filename() != kTestBinaryName)
                {
                    outError = std::string("Invalid ") + origin + " test binary '" + path.generic_string() +
                               "': the file must be named " + kTestBinaryName + ".";
                    return false;
                }
                if (!DescribeBinary(path, out, outError))
                {
                    outError = std::string("The ") + origin + " test binary does not exist: " + path.generic_string();
                    return false;
                }
                out.Origin = origin;
                return true;
            };

            if (args.contains("binary"))
            {
                if (!args["binary"].is_string() || args["binary"].get<std::string>().empty())
                {
                    outError = "Invalid 'binary': expected a non-empty path string.";
                    return false;
                }
                return fromExplicitPath(args["binary"].get<std::string>(), "argument");
            }
            // Through the engine's one environment reader (Core/Environment.h),
            // which also treats an empty variable as unset — never raw getenv.
            if (const std::optional<std::string> fromEnv = Env::Get("OLO_TESTS_BINARY"); fromEnv.has_value())
                return fromExplicitPath(*fromEnv, "environment");

            ResolvedBinary newest;
            std::string newestStamp;
            for (const fs::path& candidate : CandidateBinaries(repoRoot))
            {
                std::error_code ec;
                if (!fs::is_regular_file(candidate, ec))
                    continue;
                outSearched.push_back(candidate.generic_string());
                ResolvedBinary described;
                std::string ignored;
                if (!DescribeBinary(candidate, described, ignored))
                    continue;
                // Lexicographic on an ISO-8601 UTC stamp is chronological.
                if (newest.Path.empty() || described.ModifiedUtc > newestStamp)
                {
                    newest = described;
                    newestStamp = described.ModifiedUtc;
                }
            }
            if (newest.Path.empty())
            {
                std::string looked;
                for (const fs::path& candidate : CandidateBinaries(repoRoot))
                {
                    if (!looked.empty())
                        looked += ", ";
                    looked += candidate.generic_string();
                }
                outError = "No " + std::string(kTestBinaryName) +
                           " found. Build it first (target OloEngine-Tests), pass 'binary', or set "
                           "OLO_TESTS_BINARY. Looked in: " +
                           looked;
                return false;
            }
            newest.Origin = "search";
            out = newest;
            return true;
        }

        // ---- the child process --------------------------------------------

        enum class ChildOutcome : u8
        {
            Exited = 0,  // ran to completion; ExitCode is meaningful
            SpawnFailed, // never started
            TimedOut,    // killed at the deadline
            Cancelled,   // killed because the caller cancelled the call
        };

        struct ChildResult
        {
            ChildOutcome Outcome = ChildOutcome::SpawnFailed;
            int ExitCode = -1;
            std::string Error;   // set for SpawnFailed
            std::string Console; // whatever the child wrote before it stopped

            [[nodiscard]] bool Completed() const
            {
                return Outcome == ChildOutcome::Exited;
            }
        };

        [[nodiscard]] const char* ChildOutcomeName(ChildOutcome outcome)
        {
            switch (outcome)
            {
                case ChildOutcome::Exited:
                    return "exited";
                case ChildOutcome::SpawnFailed:
                    return "spawn-failed";
                case ChildOutcome::TimedOut:
                    return "timed-out";
                case ChildOutcome::Cancelled:
                    return "cancelled";
            }
            return "unknown";
        }

        // Emit progress from the child's console as it grows, and answer whether
        // the call has been cancelled. Shared by both platform branches so the
        // two cannot drift on when a run is abandoned.
        struct RunWatch
        {
            IAutomationHost* Host = nullptr;
            fs::path ConsolePath;
            // 0 means "no progress to report" — the listing pass, which prints no
            // per-case banners and would have the watch re-read a file for nothing.
            sizet ExpectedCases = 0;

            // Returns true when the caller has cancelled and the child should die.
            [[nodiscard]] bool Tick()
            {
                if (Host == nullptr)
                    return false;
                if (ExpectedCases > 0)
                    ReportProgress();
                return Host->IsCurrentCallCancelled();
            }

          private:
            // Count banners in what the child APPENDED since the last tick, not
            // in the whole log. A full 7000-case run writes tens of megabytes and
            // this fires four times a second for up to two hours; re-reading and
            // re-scanning all of it every time is quadratic in the log size for
            // no new information.
            void ReportProgress()
            {
                std::error_code ec;
                const auto size = static_cast<sizet>(fs::file_size(ConsolePath, ec));
                if (ec || size <= m_ScannedBytes)
                    return;

                // Re-read the last few bytes of the previous window as well, so a
                // banner split across two reads is still counted exactly once.
                const sizet overlap = std::min(m_ScannedBytes, kBannerOverlap);
                const sizet from = m_ScannedBytes - overlap;

                std::ifstream stream(ConsolePath, std::ios::binary);
                if (!stream)
                    return;
                stream.seekg(static_cast<std::streamoff>(from));
                std::string chunk(size - from, '\0');
                stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                chunk.resize(static_cast<sizet>(stream.gcount()));

                const sizet before = CountStartedCases(std::string_view(chunk).substr(0, overlap));
                m_Started += CountStartedCases(chunk) - before;
                m_ScannedBytes = from + chunk.size();

                if (const sizet started = std::min(m_Started, ExpectedCases); started > m_LastReported)
                {
                    m_LastReported = started;
                    Host->EmitProgress(static_cast<f64>(started), static_cast<f64>(ExpectedCases),
                                       "ran " + std::to_string(started) + "/" + std::to_string(ExpectedCases) +
                                           " test cases");
                }
            }

            // One banner is 12 characters; a comfortable multiple of that covers
            // any split without re-counting a banner wholly inside the old window.
            static constexpr sizet kBannerOverlap = 64;

            sizet m_ScannedBytes = 0;
            sizet m_Started = 0;
            sizet m_LastReported = 0;
        };

#ifdef _WIN32
        // One command-line string, each argument quoted only when it needs it.
        // gtest arguments carry no spaces or quotes; the executable path can.
        [[nodiscard]] std::string BuildCommandLine(const fs::path& exe, const std::vector<std::string>& arguments)
        {
            std::string command = "\"" + exe.string() + "\"";
            for (const std::string& argument : arguments)
            {
                command.push_back(' ');
                if (argument.find_first_of(" \t\"") == std::string::npos)
                    command += argument;
                else
                    command += "\"" + argument + "\"";
            }
            return command;
        }

        [[nodiscard]] std::wstring Widen(const std::string& utf8)
        {
            if (utf8.empty())
                return {};
            const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
            if (length <= 0)
                return {};
            std::wstring wide(static_cast<sizet>(length), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length);
            wide.resize(static_cast<sizet>(length - 1)); // drop the NUL the API appended
            return wide;
        }

        [[nodiscard]] ChildResult RunChild(const fs::path& exe, const std::vector<std::string>& arguments,
                                           const fs::path& workingDirectory, const fs::path& consolePath,
                                           std::chrono::milliseconds timeout, RunWatch& watch)
        {
            ChildResult result;

            SECURITY_ATTRIBUTES inheritable{};
            inheritable.nLength = sizeof(inheritable);
            inheritable.bInheritHandle = TRUE;

            // stdout and stderr both go to one file rather than a pipe: a pipe
            // nobody drains fills its buffer and BLOCKS the child, which for a
            // chatty 7000-case run is a hang that looks exactly like a slow test.
            // FILE_SHARE_READ is what lets the progress watch read the log while
            // the child is still writing it.
            const HANDLE console = ::CreateFileW(Widen(consolePath.string()).c_str(), GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_ALWAYS,
                                                 FILE_ATTRIBUTE_NORMAL, nullptr);
            if (console == INVALID_HANDLE_VALUE)
            {
                result.Error = "Could not create the console log " + consolePath.generic_string() + " (error " +
                               std::to_string(::GetLastError()) + ").";
                return result;
            }

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = nullptr;
            startup.hStdOutput = console;
            startup.hStdError = console;

            std::wstring commandLine = Widen(BuildCommandLine(exe, arguments));
            const std::wstring directory = Widen(workingDirectory.string());

            PROCESS_INFORMATION process{};
            if (!::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                  directory.empty() ? nullptr : directory.c_str(), &startup, &process))
            {
                const DWORD error = ::GetLastError();
                ::CloseHandle(console);
                result.Error = "Could not start " + exe.generic_string() + " (CreateProcess error " +
                               std::to_string(error) + ").";
                return result;
            }
            // Our copy of the write handle must go, or the log would stay open
            // for writing after the child exits.
            ::CloseHandle(console);

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            result.Outcome = ChildOutcome::Exited;
            for (;;)
            {
                const DWORD waited = ::WaitForSingleObject(process.hProcess, static_cast<DWORD>(kPollInterval.count()));
                if (waited == WAIT_OBJECT_0)
                    break;
                if (watch.Tick())
                {
                    result.Outcome = ChildOutcome::Cancelled;
                    ::TerminateProcess(process.hProcess, 1);
                    ::WaitForSingleObject(process.hProcess, 5000);
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    result.Outcome = ChildOutcome::TimedOut;
                    ::TerminateProcess(process.hProcess, 1);
                    ::WaitForSingleObject(process.hProcess, 5000);
                    break;
                }
            }

            DWORD exitCode = 0;
            if (::GetExitCodeProcess(process.hProcess, &exitCode))
                result.ExitCode = static_cast<int>(exitCode);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);

            bool ok = false;
            result.Console = ReadWholeFile(consolePath, ok);
            return result;
        }
#else
        [[nodiscard]] ChildResult RunChild(const fs::path& exe, const std::vector<std::string>& arguments,
                                           const fs::path& workingDirectory, const fs::path& consolePath,
                                           std::chrono::milliseconds timeout, RunWatch& watch)
        {
            ChildResult result;

            // fork + exec rather than posix_spawn: redirecting to a file AND
            // setting the working directory needs posix_spawn_file_actions_addchdir_np,
            // which is a glibc extension behind _GNU_SOURCE. The child below does
            // only async-signal-safe calls between fork and exec.
            std::vector<std::string> owned;
            owned.push_back(exe.string());
            owned.insert(owned.end(), arguments.begin(), arguments.end());
            std::vector<char*> argv;
            argv.reserve(owned.size() + 1);
            for (std::string& argument : owned)
                argv.push_back(argument.data());
            argv.push_back(nullptr);

            const std::string exePath = exe.string();
            const std::string consoleFile = consolePath.string();
            const std::string directory = workingDirectory.string();

            const pid_t pid = ::fork();
            if (pid < 0)
            {
                result.Error = "Could not fork to start " + exe.generic_string() + ".";
                return result;
            }
            if (pid == 0)
            {
                // Same reasoning as the Windows branch: one file, not a pipe —
                // an undrained pipe would block the child mid-run.
                if (::chdir(directory.c_str()) != 0)
                    ::_exit(127);
                const int log = ::open(consoleFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (log < 0)
                    ::_exit(127);
                (void)::dup2(log, STDOUT_FILENO);
                (void)::dup2(log, STDERR_FILENO);
                if (log > STDERR_FILENO)
                    (void)::close(log);
                ::execv(exePath.c_str(), argv.data());
                ::_exit(127);
            }

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            result.Outcome = ChildOutcome::Exited;
            for (;;)
            {
                int status = 0;
                const pid_t waited = ::waitpid(pid, &status, WNOHANG);
                if (waited == pid)
                {
                    result.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                    break;
                }
                if (watch.Tick())
                {
                    result.Outcome = ChildOutcome::Cancelled;
                    ::kill(pid, SIGKILL);
                    (void)::waitpid(pid, &status, 0);
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    result.Outcome = ChildOutcome::TimedOut;
                    ::kill(pid, SIGKILL);
                    (void)::waitpid(pid, &status, 0);
                    break;
                }
                std::this_thread::sleep_for(kPollInterval);
            }

            bool ok = false;
            result.Console = ReadWholeFile(consolePath, ok);
            return result;
        }
#endif

        // ---- classification index -----------------------------------------

        // Resolves a test file's OLO_TEST_LAYER, from the in-file marker first
        // and test_catalogue.json's file_layer_map second, caching per file so a
        // 7000-case listing reads each source once. Every classification defect
        // it meets is collected in Problems and REPORTED with the result rather
        // than smoothed into a blank layer.
        class LayerIndex
        {
          public:
            explicit LayerIndex(const fs::path& repoRoot) : m_RepoRoot(repoRoot)
            {
                bool ok = false;
                const std::string text = ReadWholeFile(repoRoot / kCatalogueRelative, ok);
                if (!ok)
                {
                    m_Problems.push_back(std::string("Could not read ") + kCatalogueRelative +
                                         "; no test is classified.");
                    return;
                }
                const Json catalogue = Json::parse(text, nullptr, false);
                if (catalogue.is_discarded())
                {
                    m_Problems.push_back(std::string(kCatalogueRelative) + " is not valid JSON; no test is classified.");
                    return;
                }
                if (catalogue.contains("file_layer_map"))
                    m_FileLayerMap = catalogue["file_layer_map"];
                m_KnownIds = KnownLayerIds(catalogue);
            }

            // `rawFile` is gtest's `file` field. Returns the layer, or empty when
            // the file could not be classified (the reason lands in Problems).
            [[nodiscard]] std::string LayerFor(const std::string& rawFile, std::string& outRepoRelative)
            {
                outRepoRelative = NormalizeTestFile(rawFile, kTestRoot);
                if (outRepoRelative.empty())
                {
                    NoteProblem("Test source '" + rawFile + "' is not under " + kTestRoot +
                                "; it cannot be classified.");
                    return {};
                }
                if (const auto cached = m_Cache.find(outRepoRelative); cached != m_Cache.end())
                    return cached->second;

                bool ok = false;
                const std::string text = ReadWholeFile(m_RepoRoot / outRepoRelative, ok);
                const LayerMarker marker = ok ? ExtractLayerMarker(text) : LayerMarker{};
                if (!ok)
                    NoteProblem("Could not read " + outRepoRelative + " to look for its // OLO_TEST_LAYER marker.");

                const LayerResolution resolved = ResolveLayer(outRepoRelative, marker, m_FileLayerMap, m_KnownIds);
                if (!resolved.Problem.empty())
                    NoteProblem(resolved.Problem);
                m_Cache.emplace(outRepoRelative, resolved.Layer);
                return resolved.Layer;
            }

            [[nodiscard]] const std::vector<std::string>& Problems() const
            {
                return m_Problems;
            }

          private:
            void NoteProblem(std::string problem)
            {
                if (std::find(m_Problems.begin(), m_Problems.end(), problem) == m_Problems.end())
                    m_Problems.push_back(std::move(problem));
            }

            fs::path m_RepoRoot;
            Json m_FileLayerMap = Json::object();
            std::set<std::string> m_KnownIds;
            std::unordered_map<std::string, std::string> m_Cache;
            std::vector<std::string> m_Problems;
        };

        // ---- shared selection plumbing ------------------------------------

        // A scratch directory for one invocation's gtest report + console log,
        // removed when the invocation ends however it ends.
        class ScratchDirectory
        {
          public:
            explicit ScratchDirectory(const char* stem)
            {
                std::error_code ec;
                const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                m_Path = fs::temp_directory_path(ec) /
                         (std::string("olo-") + stem + "-" + std::to_string(static_cast<u64>(stamp)));
                if (!ec)
                    fs::create_directories(m_Path, ec);
                if (ec)
                    m_Path.clear();
            }
            ~ScratchDirectory()
            {
                if (m_Path.empty())
                    return;
                std::error_code ec;
                fs::remove_all(m_Path, ec);
            }

            ScratchDirectory(const ScratchDirectory&) = delete;
            ScratchDirectory& operator=(const ScratchDirectory&) = delete;
            ScratchDirectory(ScratchDirectory&&) = delete;
            ScratchDirectory& operator=(ScratchDirectory&&) = delete;

            [[nodiscard]] bool Valid() const
            {
                return !m_Path.empty();
            }
            [[nodiscard]] fs::path File(const char* name) const
            {
                return m_Path / name;
            }

          private:
            fs::path m_Path;
        };

        // Everything a run or a listing needs, resolved once.
        struct Session
        {
            fs::path RepoRoot;
            ResolvedBinary Binary;
            // Every existing candidate the search considered. Reported only when
            // there was more than one: that is the case where "newest wins" made
            // a choice, and a caller staring at an unexpected result deserves to
            // see the tree it did not get.
            Json Candidates = Json::array();
        };

        [[nodiscard]] bool OpenSession(const Json& args, Session& out, std::string& outError)
        {
            if (!FindRepoRoot(out.RepoRoot, outError))
                return false;
            return ResolveBinary(args, out.RepoRoot, out.Binary, out.Candidates, outError);
        }

        // Stamp the resolved binary (and, when ambiguous, the trees it beat) onto
        // a result payload. Shared so both commands report provenance the same way.
        void DescribeSession(const Session& session, Json& out)
        {
            out["binary"] = BinaryJson(session.Binary);
            out["repoRoot"] = session.RepoRoot.generic_string();
            if (session.Candidates.is_array() && session.Candidates.size() > 1)
                out["binaryCandidates"] = session.Candidates;
        }

        // Turn the caller's selection arguments into one gtest filter.
        // `suite` and `cases` are conveniences over `filter`; giving more than
        // one of the three is refused rather than silently ranked, because the
        // caller who did it meant something specific and cannot tell which won.
        [[nodiscard]] bool BuildSelectionFilter(const Json& args, std::string& outFilter, std::string& outError)
        {
            int given = 0;
            for (const char* key : { "filter", "suite", "cases" })
                given += args.contains(key) ? 1 : 0;
            if (given > 1)
            {
                outError = "Give at most one of 'filter', 'suite' or 'cases' — they are three spellings of the same "
                           "selection and cannot be combined.";
                return false;
            }

            if (args.contains("filter"))
            {
                if (!args["filter"].is_string() || !IsFilterSafeExpression(args["filter"].get<std::string>()))
                {
                    // Charset-checked, not quoted-around: the filter is pasted
                    // into the child's command line and a quote in it would let
                    // the rest be read as further gtest flags.
                    outError = "Invalid 'filter': expected a non-empty gtest filter expression of "
                               "[A-Za-z0-9_./*?:-] (a gtest filter has no escape syntax, so nothing else is "
                               "meaningful in one).";
                    return false;
                }
                outFilter = args["filter"].get<std::string>();
            }
            else if (args.contains("suite"))
            {
                if (!args["suite"].is_string() || !IsFilterSafeName(args["suite"].get<std::string>()))
                {
                    outError = "Invalid 'suite': expected a test suite name ([A-Za-z0-9_./]).";
                    return false;
                }
                outFilter = args["suite"].get<std::string>() + ".*";
            }
            else if (args.contains("cases"))
            {
                if (!args["cases"].is_array() || args["cases"].empty())
                {
                    outError = "Invalid 'cases': expected a non-empty array of 'Suite.Case' names.";
                    return false;
                }
                std::vector<std::string> names;
                for (const auto& entry : args["cases"])
                {
                    if (!entry.is_string() || !IsFilterSafeName(entry.get<std::string>()))
                    {
                        outError = "Invalid entry in 'cases': expected 'Suite.Case' names of [A-Za-z0-9_./].";
                        return false;
                    }
                    names.push_back(entry.get<std::string>());
                }
                outFilter = JoinFilter(names);
            }
            else
            {
                outFilter = "*";
            }

            if (outFilter.size() > kMaxFilterChars)
            {
                outError = "The selection expands to a " + std::to_string(outFilter.size()) +
                           "-character gtest filter, over the " + std::to_string(kMaxFilterChars) +
                           "-character ceiling one command line can carry. Narrow it.";
                return false;
            }
            return true;
        }

        // Run the LIST pass: what the selection actually names, classified.
        // This is both the answer olo_tests_list returns and the expected set
        // olo_tests_run reconciles against.
        [[nodiscard]] bool ListSelection(const Session& session, const std::string& filter, IAutomationHost& host,
                                         std::vector<TestCase>& outCases, LayerIndex& layers, std::string& outError)
        {
            const ScratchDirectory scratch("tests-list");
            if (!scratch.Valid())
            {
                outError = "Could not create a temporary directory for the gtest listing.";
                return false;
            }
            const fs::path reportPath = scratch.File("list.json");
            const fs::path consolePath = scratch.File("list.log");

            RunWatch watch;
            watch.Host = &host;
            watch.ConsolePath = consolePath;

            const std::vector<std::string> arguments{ "--gtest_list_tests", "--gtest_filter=" + filter,
                                                      "--gtest_output=json:" + reportPath.string() };
            const ChildResult child = RunChild(session.Binary.Path, arguments, session.RepoRoot / kChildWorkingDirectory, consolePath,
                                               std::chrono::seconds(300), watch);
            if (!child.Completed())
            {
                outError = std::string("Listing the test cases ") + ChildOutcomeName(child.Outcome) + ". " +
                           child.Error + (child.Console.empty() ? "" : "\n" + TailOf(child.Console, kConsoleTailChars));
                return false;
            }

            bool ok = false;
            const std::string reportText = ReadWholeFile(reportPath, ok);
            if (!ok)
            {
                outError = "The test binary exited with code " + std::to_string(child.ExitCode) +
                           " without writing its listing to " + reportPath.generic_string() + ".\n" +
                           TailOf(child.Console, kConsoleTailChars);
                return false;
            }
            const Json report = Json::parse(reportText, nullptr, false);
            if (report.is_discarded())
            {
                outError = "The gtest listing at " + reportPath.generic_string() + " is not valid JSON.";
                return false;
            }
            outCases = ParseListReport(report, outError);
            if (!outError.empty())
                return false;

            for (TestCase& testCase : outCases)
            {
                std::string repoRelative;
                testCase.Layer = layers.LayerFor(testCase.File, repoRelative);
                testCase.File = repoRelative;
            }
            return true;
        }

        // Expand a layer-narrowed selection into a gtest filter.
        //
        // A layer is not expressible as a gtest pattern, so it has to become an
        // explicit name list — and `layer:"unit"` alone is ~3000 names, ~135 KB,
        // four times what one Windows command line can carry. So a suite whose
        // cases ALL survived the narrowing collapses to `Suite.*`, which is what
        // makes a whole-layer run possible at all.
        //
        // The collapse can only ever be exact: `listed` is the complete listing
        // the same filter produced, so "every listed case of this suite
        // survived" means the suite has no unselected case to widen into. The
        // caller must still pass `listedIsEverything` — with a base filter
        // narrower than `*`, `listed` is already a subset and `Suite.*` would
        // reach cases the caller excluded. And if the collapse were ever wrong,
        // the run's own reconciliation would name the extra cases rather than
        // let them pass as part of the result.
        [[nodiscard]] std::string BuildLayerFilter(const std::vector<TestCase>& listed,
                                                   const std::vector<TestCase>& selected, bool listedIsEverything)
        {
            std::unordered_map<std::string, sizet> listedPerSuite;
            for (const TestCase& testCase : listed)
                ++listedPerSuite[testCase.Suite];
            std::unordered_map<std::string, sizet> selectedPerSuite;
            for (const TestCase& testCase : selected)
                ++selectedPerSuite[testCase.Suite];

            std::vector<std::string> terms;
            std::set<std::string> collapsed;
            for (const TestCase& testCase : selected)
            {
                if (listedIsEverything && selectedPerSuite[testCase.Suite] == listedPerSuite[testCase.Suite])
                {
                    if (collapsed.insert(testCase.Suite).second)
                        terms.push_back(testCase.Suite + ".*");
                    continue;
                }
                terms.push_back(testCase.FullName());
            }
            return JoinFilter(terms);
        }

        // The one place "the selection matched nothing" is turned into an error.
        // A caller that reads `0 cases, 0 failures` out of a typo'd filter has
        // been told its code is fine by a run that never happened.
        [[nodiscard]] std::string EmptySelectionError(const std::string& filter, const Session& session)
        {
            return "The selection matched no test cases (filter '" + filter + "' against " +
                   session.Binary.Path.generic_string() +
                   "). That is an error, not an empty pass: check the suite spelling with olo_tests_list, or rebuild "
                   "the binary if the test is new.";
        }

        // ---- olo_tests_list -----------------------------------------------

        ToolResult Handle_TestsList(IAutomationHost& host, const Json& args)
        {
            Session session;
            if (std::string error; !OpenSession(args, session, error))
                return ToolResult::Error(error);

            std::string filter;
            if (std::string error; !BuildSelectionFilter(args, filter, error))
                return ToolResult::Error(error);

            LayerIndex layers(session.RepoRoot);
            std::vector<TestCase> cases;
            if (std::string error; !ListSelection(session, filter, host, cases, layers, error))
                return ToolResult::Error(error);

            std::string layerFilter;
            if (args.contains("layer"))
            {
                if (!args["layer"].is_string() || args["layer"].get<std::string>().empty())
                    return ToolResult::Error("Invalid 'layer': expected a classification id such as 'L1' or 'unit'.");
                layerFilter = args["layer"].get<std::string>();
                std::erase_if(cases, [&layerFilter](const TestCase& c) { return c.Layer != layerFilter; });
            }

            if (cases.empty())
                return ToolResult::Error(layerFilter.empty()
                                             ? EmptySelectionError(filter, session)
                                             : "No test case in the selection carries layer '" + layerFilter +
                                                   "'. That is an error, not an empty listing: check the id against "
                                                   "test_catalogue.json's 'layers'.");

            int page = 0;
            int pageSize = 100;
            if (args.contains("page") && args["page"].is_number_integer())
                page = static_cast<int>(std::max<long long>(0, args["page"].get<long long>()));
            if (args.contains("pageSize") && args["pageSize"].is_number_integer())
                pageSize = static_cast<int>(std::clamp<long long>(args["pageSize"].get<long long>(), 1, 500));

            Json out;
            DescribeSession(session, out);
            out["filter"] = filter;
            if (!layerFilter.empty())
                out["layer"] = layerFilter;
            out["total"] = cases.size();
            out["layerCounts"] = LayerCountsJson(cases);

            const auto total = static_cast<long long>(cases.size());
            const long long start = static_cast<long long>(page) * pageSize;
            Json entries = Json::array();
            for (long long i = start; i < total && i < start + pageSize; ++i)
            {
                const TestCase& testCase = cases[static_cast<sizet>(i)];
                Json entry;
                entry["suite"] = testCase.Suite;
                entry["name"] = testCase.Name;
                entry["fullName"] = testCase.FullName();
                if (!testCase.File.empty())
                {
                    entry["file"] = testCase.File;
                    entry["line"] = testCase.Line;
                }
                if (!testCase.Layer.empty())
                    entry["layer"] = testCase.Layer;
                if (testCase.Disabled)
                    entry["disabled"] = true;
                entries.push_back(std::move(entry));
            }
            out["page"] = page;
            out["pageSize"] = pageSize;
            out["returned"] = entries.size();
            if (start + pageSize < total)
                out["nextPage"] = page + 1;
            out["cases"] = std::move(entries);

            // Classification defects ride along instead of being logged and
            // forgotten: an unclassified test file is invisible to every
            // layer-scoped selection, which is exactly the silent partial this
            // command exists to refuse.
            if (!layers.Problems().empty())
            {
                out["classificationProblems"] = layers.Problems();
                out["classificationProblemCount"] = layers.Problems().size();
            }
            return ToolResult::Structured(out);
        }

        // ---- olo_tests_run ------------------------------------------------

        ToolResult Handle_TestsRun(IAutomationHost& host, const Json& args)
        {
            Session session;
            if (std::string error; !OpenSession(args, session, error))
                return ToolResult::Error(error);

            std::string filter;
            if (std::string error; !BuildSelectionFilter(args, filter, error))
                return ToolResult::Error(error);

            const bool includePassed = args.value("includePassed", false);
            const auto maxCases =
                static_cast<sizet>(std::clamp<long long>(args.value("maxCases", 200LL), 1LL, 2000LL));
            const auto maxMessageChars =
                static_cast<sizet>(std::clamp<long long>(args.value("maxMessageChars", 2000LL), 200LL, 20000LL));
            const auto timeoutSeconds = std::clamp<long long>(args.value("timeoutSeconds", 1800LL), 10LL, 7200LL);

            // ---- pass 1: what did the caller select? ----
            LayerIndex layers(session.RepoRoot);
            std::vector<TestCase> listed;
            if (std::string error; !ListSelection(session, filter, host, listed, layers, error))
                return ToolResult::Error(error);

            std::vector<TestCase> selected = listed;
            if (args.contains("layer"))
            {
                if (!args["layer"].is_string() || args["layer"].get<std::string>().empty())
                    return ToolResult::Error("Invalid 'layer': expected a classification id such as 'L1' or 'unit'.");
                const std::string layerFilter = args["layer"].get<std::string>();
                std::erase_if(selected, [&layerFilter](const TestCase& c) { return c.Layer != layerFilter; });
                if (selected.empty())
                    return ToolResult::Error("No test case in the selection carries layer '" + layerFilter +
                                             "'. That is an error, not an empty run.");
                for (const TestCase& testCase : selected)
                {
                    if (!IsFilterSafeName(testCase.FullName()))
                        return ToolResult::Error("Test case '" + testCase.FullName() +
                                                 "' cannot be named in a gtest filter; select it with 'filter'.");
                }
                filter = BuildLayerFilter(listed, selected, /*listedIsEverything*/ filter == "*");
                if (filter.size() > kMaxFilterChars)
                    return ToolResult::Error(
                        "Layer '" + layerFilter + "' selects " + std::to_string(selected.size()) +
                        " cases, which expand to a " + std::to_string(filter.size()) +
                        "-character gtest filter — over the " + std::to_string(kMaxFilterChars) +
                        "-character ceiling one command line can carry. Narrow it with 'suite' or 'filter' as well.");
            }

            if (selected.empty())
                return ToolResult::Error(EmptySelectionError(filter, session));

            // ---- pass 2: run it ----
            const ScratchDirectory scratch("tests-run");
            if (!scratch.Valid())
                return ToolResult::Error("Could not create a temporary directory for the gtest report.");
            const fs::path reportPath = scratch.File("report.json");
            const fs::path consolePath = scratch.File("console.log");

            std::vector<std::string> arguments{ "--gtest_filter=" + filter,
                                                "--gtest_output=json:" + reportPath.string() };
            if (args.value("shuffle", false))
            {
                arguments.emplace_back("--gtest_shuffle");
                if (args.contains("seed") && args["seed"].is_number_integer())
                    arguments.push_back("--gtest_random_seed=" +
                                        std::to_string(std::clamp<long long>(args["seed"].get<long long>(), 0, 99999)));
            }
            if (args.value("alsoRunDisabled", false))
                arguments.emplace_back("--gtest_also_run_disabled_tests");

            RunWatch watch;
            watch.Host = &host;
            watch.ConsolePath = consolePath;
            watch.ExpectedCases = selected.size();

            const auto started = std::chrono::steady_clock::now();
            const ChildResult child = RunChild(session.Binary.Path, arguments, session.RepoRoot / kChildWorkingDirectory, consolePath,
                                               std::chrono::seconds(timeoutSeconds), watch);
            const f64 wallSeconds =
                std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();

            // Everything the caller needs to see even when the run collapsed.
            const auto failure = [&](const std::string& what) -> ToolResult
            {
                Json detail;
                detail["binary"] = BinaryJson(session.Binary);
                detail["filter"] = filter;
                detail["selected"] = selected.size();
                detail["outcome"] = ChildOutcomeName(child.Outcome);
                detail["exitCode"] = child.ExitCode;
                detail["startedCases"] = CountStartedCases(child.Console);
                detail["wallSeconds"] = wallSeconds;
                return ToolResult::Error(what + "\n\n" + detail.dump(2) + "\n\nConsole tail:\n" +
                                         TailOf(child.Console, kConsoleTailChars));
            };

            if (!child.Completed())
                return failure("The test run " + std::string(ChildOutcomeName(child.Outcome)) +
                               " and produced no complete report. " + child.Error);

            bool ok = false;
            const std::string reportText = ReadWholeFile(reportPath, ok);
            if (!ok)
                return failure("The test binary exited with code " + std::to_string(child.ExitCode) +
                               " without writing a report — it crashed or aborted mid-run, so no result here would "
                               "account for the whole selection.");
            const Json report = Json::parse(reportText, nullptr, false);
            if (report.is_discarded())
                return failure("The gtest report is not valid JSON — the run was cut short while writing it.");

            RunReport parsed = ParseRunReport(report);
            if (!parsed.Error.empty())
                return failure(parsed.Error);

            // ---- pass 3: does the report account for the whole selection? ----
            const Reconciliation reconciliation = Reconcile(selected, parsed.Cases);
            if (!reconciliation.Complete())
            {
                Json detail;
                detail["expected"] = reconciliation.Expected;
                detail["reported"] = reconciliation.Reported;
                detail["missing"] = reconciliation.Missing;
                detail["unexpected"] = reconciliation.Unexpected;
                return failure("The run was PARTIAL: the report does not account for every selected case.\n" +
                               detail.dump(2));
            }

            // Carry the classification across from the listing.
            std::unordered_map<std::string, const TestCase*> byName;
            for (const TestCase& testCase : selected)
                byName.emplace(testCase.FullName(), &testCase);
            for (CaseResult& result : parsed.Cases)
            {
                if (const auto found = byName.find(result.FullName()); found != byName.end())
                {
                    result.Layer = found->second->Layer;
                    result.File = found->second->File;
                    result.Line = found->second->Line;
                }
            }

            const RunCounts counts = CountCases(parsed.Cases);
            if (!counts.AddsUp())
                return failure("Internal error: the per-status counts (" + std::to_string(counts.Passed) + " passed, " +
                               std::to_string(counts.Failed) + " failed, " + std::to_string(counts.Skipped) +
                               " skipped, " + std::to_string(counts.Disabled) + " disabled) do not sum to the " +
                               std::to_string(counts.Total) + " reported cases.");

            Json out;
            DescribeSession(session, out);
            out["filter"] = filter;
            out["exitCode"] = child.ExitCode;
            out["wallSeconds"] = wallSeconds;
            out["passed"] = counts.Passed;
            out["failed"] = counts.Failed;
            out["skipped"] = counts.Skipped;
            out["disabled"] = counts.Disabled;
            out["total"] = counts.Total;
            // The whole selection ran and every case is accounted for. This flag
            // is only ever true alongside the reconciliation below, so a caller
            // can trust one number instead of re-deriving the invariant.
            out["complete"] = true;
            out["reconciliation"] = Json{ { "expected", reconciliation.Expected },
                                          { "reported", reconciliation.Reported },
                                          { "missing", Json::array() },
                                          { "unexpected", Json::array() } };

            Json cases = Json::array();
            sizet omitted = 0;
            for (const CaseResult& result : parsed.Cases)
            {
                if (!includePassed && result.Status == CaseStatus::Passed)
                    continue;
                if (cases.size() >= maxCases)
                {
                    ++omitted;
                    continue;
                }
                cases.push_back(CaseJson(result, maxMessageChars));
            }
            out["cases"] = std::move(cases);
            out["casesIncludePassed"] = includePassed;

            // A fatal failure in SetUpTestSuite / TearDownTestSuite / a global
            // environment belongs to no case, so it is reported here rather than
            // being counted among them — and, crucially, it is reported at all:
            // read as a case it would have reconciled as `unexpected` and turned
            // a real failure into a "the run was PARTIAL" complaint.
            Json suiteFailures = Json::array();
            for (const SuiteFailure& suiteFailure : parsed.SuiteFailures)
            {
                Json entry;
                entry["suite"] = suiteFailure.Suite;
                Json messages = Json::array();
                for (const std::string& message : suiteFailure.Messages)
                    messages.push_back(Clamp(message, maxMessageChars));
                entry["messages"] = std::move(messages);
                suiteFailures.push_back(std::move(entry));
            }
            out["suiteFailureCount"] = suiteFailures.size();
            if (!suiteFailures.empty())
                out["suiteFailures"] = std::move(suiteFailures);
            // Truncation is stated and counted rather than left to be inferred
            // from an array that happens to be exactly maxCases long.
            out["casesOmitted"] = omitted;
            if (omitted > 0)
                out["casesTruncated"] = true;

            if (!layers.Problems().empty())
            {
                out["classificationProblems"] = layers.Problems();
                out["classificationProblemCount"] = layers.Problems().size();
            }

            OLO_CORE_INFO("[MCP] olo_tests_run: {} passed, {} failed, {} skipped, {} disabled in {:.1f}s ({})",
                          counts.Passed, counts.Failed, counts.Skipped, counts.Disabled, wallSeconds,
                          session.Binary.Path.generic_string());
            return ToolResult::Structured(out);
        }

        // ---- schema fragments shared by both commands ----------------------

        [[nodiscard]] Schema::Node SelectionProps(Schema::Node node)
        {
            return node
                .Prop("filter", Schema::String().Desc(
                                    "gtest filter expression, e.g. 'RenderGraphTest.*' or 'A.B:C.*-*Slow*'. "
                                    "Mutually exclusive with 'suite' and 'cases'; omit all three for every case."))
                .Prop("suite", Schema::String().Desc("A single suite name, shorthand for the filter '<suite>.*'."))
                .Prop("cases", Schema::Array(Schema::String())
                                   .Desc("Explicit 'Suite.Case' full names to select."))
                .Prop("layer", Schema::String().Desc(
                                   "Keep only cases carrying this OLO_TEST_LAYER classification (L1-L11, "
                                   "plumbing, cullinglod, shaderpipe, integration, meta, Functional, unit). "
                                   "An id that matches nothing in the selection is an error."))
                .Prop("binary", Schema::String().Desc(
                                    "Path to OloEngine-Tests (absolute, or relative to the repo root). Defaults "
                                    "to OLO_TESTS_BINARY, then the newest build tree under the repo root."));
        }
    } // namespace

    void RegisterTestingTools(AutomationRegistry& registry)
    {
        {
            ToolDef tool;
            tool.Name = "olo_tests_list";
            tool.Toolset = "tests";
            tool.Title = "List test cases";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Enumerate the GoogleTest cases OloEngine-Tests holds, with each case's source file, line and "
                "OLO_TEST_LAYER classification (the renderer pyramid L1-L11 and the Functional / unit axes). "
                "Filter by gtest expression, suite, explicit names or layer. A selection that matches nothing is "
                "an ERROR, not an empty list.";
            tool.InputSchema = SelectionProps(Schema::Object())
                                   .Pagination("Cases per page (default 100, max 500).")
                                   .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("binary", Schema::Object().Desc("Which OloEngine-Tests was enumerated, and how old it is."))
                    .Prop("repoRoot", Schema::String())
                    .Prop("filter", Schema::String().Desc("The gtest filter that was applied."))
                    .Prop("total", Schema::Int().Min(1).Desc("Cases the selection matched (never 0 — that errors)."))
                    .Prop("layerCounts", Schema::Object().Desc("Case count per OLO_TEST_LAYER id; unclassified "
                                                               "cases are counted under 'unclassified'."))
                    .Prop("page", Schema::Int().Min(0))
                    .Prop("pageSize", Schema::Int().Min(1))
                    .Prop("returned", Schema::Int().Min(0))
                    .Prop("nextPage", Schema::Int().Min(1).Desc("Omitted on the last page."))
                    .Prop("cases", Schema::Array(Schema::Object()
                                                     .Prop("suite", Schema::String())
                                                     .Prop("name", Schema::String())
                                                     .Prop("fullName", Schema::String())
                                                     .Prop("file", Schema::String().Desc("Repo-relative source path."))
                                                     .Prop("line", Schema::Int().Min(0))
                                                     .Prop("layer", Schema::String())
                                                     .Prop("disabled", Schema::Bool())))
                    .Prop("classificationProblems",
                          Schema::Array(Schema::String())
                              .Desc("Test files the OLO_TEST_LAYER gate would reject. Reported, never swallowed."))
                    .Required({ "binary", "repoRoot", "filter", "total", "page", "pageSize", "returned", "cases" });
            tool.Handler = Handle_TestsList;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_tests_run";
            tool.Toolset = "tests";
            tool.Title = "Run tests";
            tool.DualAudienceContent = true;
            // NOT read-only, and the distinction is not pedantic: a full run
            // regenerates the tracked evidence PNGs under
            // OloEditor/assets/tests/visual/, so the working tree moves. It is
            // still not a ProjectWrite — it mutates nothing the user is
            // AUTHORING (no scene, no component, no asset registry entry), and
            // gating verification behind the write-consent toggle would make
            // the loop this issue exists to open unusable by default.
            // destructiveHint stays false: those PNGs are generated artefacts
            // the run reproduces, not user data it destroys.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Run a filtered selection of OloEngine-Tests as a child process and return STRUCTURED per-case "
                "results — pass/fail/skip/disabled, the verbatim gtest failure text, per-case timings and each "
                "case's OLO_TEST_LAYER — with no console parsing. Does not use the editor's renderer, so the "
                "CPU-only suites do not inherit any GPU skip condition. Reports failures by default; set "
                "includePassed for the whole set. A run is never silently partial: a selection matching nothing, "
                "a child that crashed, timed out or was cancelled, and a report that does not account for every "
                "selected case are all errors that name what is missing. Note that a run regenerates the tracked "
                "evidence PNGs under OloEditor/assets/tests/visual/, so the working tree moves — never 'git add -A' "
                "after one.";
            tool.InputSchema =
                SelectionProps(Schema::Object())
                    .Prop("includePassed",
                          Schema::Bool().Desc("Include passing cases in 'cases' (default false — failures only)."))
                    .Prop("maxCases", Schema::Int().Min(1).Max(2000).Desc(
                                          "Cap on reported cases (default 200). Any excess is counted in "
                                          "'casesOmitted', never silently dropped."))
                    .Prop("maxMessageChars", Schema::Int().Min(200).Max(20000).Desc(
                                                 "Cap on one failure message (default 2000); a cut is marked."))
                    .Prop("timeoutSeconds", Schema::Int().Min(10).Max(7200).Desc(
                                                "Kill the run after this long (default 1800). A timeout is an "
                                                "error, not a partial result."))
                    .Prop("shuffle", Schema::Bool().Desc("Run in a random order (--gtest_shuffle)."))
                    .Prop("seed", Schema::Int().Min(0).Max(99999).Desc("Shuffle seed (--gtest_random_seed)."))
                    .Prop("alsoRunDisabled",
                          Schema::Bool().Desc("Also run DISABLED_ cases (--gtest_also_run_disabled_tests)."))
                    .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("binary", Schema::Object().Desc("Which OloEngine-Tests ran, its size and its build time."))
                    .Prop("repoRoot", Schema::String())
                    .Prop("filter", Schema::String())
                    .Prop("exitCode", Schema::Int().Desc("The test binary's own exit code."))
                    .Prop("wallSeconds", Schema::Number())
                    .Prop("passed", Schema::Int().Min(0))
                    .Prop("failed", Schema::Int().Min(0))
                    .Prop("skipped", Schema::Int().Min(0))
                    .Prop("disabled", Schema::Int().Min(0))
                    .Prop("total", Schema::Int().Min(1))
                    .Prop("complete", Schema::Bool().Desc("Always true on success: every selected case reported."))
                    .Prop("reconciliation", Schema::Object().Desc(
                                                "expected/reported counts and the (always empty on success) "
                                                "'missing' and 'unexpected' name lists."))
                    .Prop("cases", Schema::Array(Schema::Object()
                                                     .Prop("suite", Schema::String())
                                                     .Prop("name", Schema::String())
                                                     .Prop("fullName", Schema::String())
                                                     .Prop("status", Schema::String().Enum({ "passed", "failed",
                                                                                             "skipped", "disabled" }))
                                                     .Prop("seconds", Schema::Number())
                                                     .Prop("file", Schema::String())
                                                     .Prop("line", Schema::Int().Min(0))
                                                     .Prop("layer", Schema::String())
                                                     .Prop("messages", Schema::Array(Schema::String())
                                                                           .Desc("Verbatim gtest failure text, or "
                                                                                 "the GTEST_SKIP reason."))))
                    .Prop("casesOmitted", Schema::Int().Min(0).Desc("Cases past 'maxCases', counted not dropped."))
                    .Prop("suiteFailureCount", Schema::Int().Min(0).Desc(
                                                   "Failures belonging to no case — a fatal assertion in "
                                                   "SetUpTestSuite/TearDownTestSuite or a global environment."))
                    .Prop("suiteFailures", Schema::Array(Schema::Object()
                                                             .Prop("suite", Schema::String())
                                                             .Prop("messages", Schema::Array(Schema::String())))
                                               .Desc("Present only when suiteFailureCount > 0."))
                    .Required({ "binary", "filter", "exitCode", "passed", "failed", "skipped", "disabled", "total",
                                "complete", "reconciliation", "cases", "suiteFailureCount" });
            tool.Handler = Handle_TestsRun;
            registry.Register(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
