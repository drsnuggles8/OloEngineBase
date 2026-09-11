#include "OloEnginePCH.h"
#include "Automation/AutomationBuildCommands.h"

#include "Automation/AutomationBuildInvocation.h"
#include "Automation/AutomationCommand.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationResult.h"
#include "MCP/McpSchemaBuilder.h"

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

// Structured build invocation: olo_build_list and olo_build_run (issue #1163,
// Epic H slice 3, deferred out of #1130).
//
// THE CONCURRENCY CONTRACT, which is the substance of this slice. Full
// derivation in docs/agent-rules/automation-build-invocation.md; the four
// answers, and where each is enforced:
//
//   1. ACQUIRE, never bypass. Every build goes through build-lock.ps1, with a
//      bounded, caller-set wait (`lockWaitSeconds`, default 300) mapped onto
//      the script's own -TimeoutMinutes. The script takes a queue ticket, so we
//      never jump anyone; when the budget expires the script names the holder
//      and that becomes the error. None of OLO_BUILD_LOCK_OVERRIDE,
//      OLO_BUILD_LOCK_BYPASS or OLO_NOT_A_BUILD appears anywhere in this file:
//      this command must not become a third, unaudited way to start a build.
//
//   2. THE EDITOR PROCESS IS THE LOCK IDENTITY. build-lock.ps1's parent watch
//      reads Win32_Process.ParentProcessId of the pwsh it runs in, so whatever
//      spawns pwsh is the identity. We spawn it DIRECTLY — no cmd /c, no
//      launcher shim — so the identity is this editor, pinned by (pid,
//      StartTime), and the lock reaps the build when the editor dies.
//
//   3. CANCELLATION KILLS THE TREE, NOT THE SHIM. Killing pwsh alone would
//      release the lock (the OS closes its handle) while ninja and its
//      compilers kept running unbounded — the exact shape the lock exists to
//      prevent. So the child is created suspended, assigned to a Win32 job
//      object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, and then resumed;
//      cancelling terminates the JOB. That also covers what the parent watch
//      cannot: an editor killed outright runs no cleanup, but the OS closes the
//      job handle on process exit and the whole build dies with it. POSIX gets
//      the same shape from a process group.
//
//   4. THE EDITOR MAY NOT BUILD ITSELF, and the refusal is an ALLOW-LIST.
//      Windows keeps a running image mapped, so linking OloEditor.exe fails
//      with LNK1168 after all the compile work; Mono holds
//      OloEngine-ScriptCore's assembly. A deny-list would leak through `all` /
//      `ALL_BUILD` / `install` and through any target added later. The
//      allow-list is also the injection boundary: target, configuration and
//      build directory are interpolated into a PowerShell command string, so
//      each is resolved against a fixed table in AutomationBuildInvocation.h
//      and nothing freehand ever reaches it.
//
// AND THE RULE THE WHOLE RESULT SHAPE EXISTS FOR: a zero exit code is not
// evidence that anything was built. build-lock.ps1 stands down with exit 0 when
// an identical command was queued later; an incremental build with nothing to
// do also exits 0; and a build that never started leaves last week's binary
// exactly where a caller looks. So the verdict comes from the ARTEFACT — stat'd
// before and after, compared against the moment the build started — and the
// three cases are reported as three different outcomes, never merged into
// "success".

namespace OloEngine::Automation
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace OloEngine::Automation::BuildInvocation;
        namespace Schema = OloEngine::MCP::Schema;

        // The repo marker: the lock script itself. Anchoring on the very file we
        // need means a root we "found" without one is not a root we can build
        // from — and there is deliberately no fallback that builds anyway.
        constexpr const char* kLockScriptRelative = ".claude/skills/run-oloengine/build-lock.ps1";

        // How far up from the working directory to look. The editor runs with
        // cwd = OloEditor/, so one level suffices; the margin covers a caller
        // that launched it from deeper.
        constexpr int kMaxRootSearchDepth = 8;

        // Poll cadence while the child runs. Often enough that a cancellation is
        // acted on promptly, rarely enough that re-reading a growing log costs
        // nothing next to a compile.
        constexpr auto kPollInterval = std::chrono::milliseconds(250);

        // Console tail returned when a build fails. Big enough for a template
        // error and the ninja edges around it.
        constexpr sizet kConsoleTailChars = 8000;

        // Progress is reported on one permille scale so it stays monotonic
        // across the two phases: queueing for the lock, then building. Queueing
        // gets the first 5% — it is a wait, not work.
        constexpr i64 kProgressTotal = 1000;
        constexpr i64 kProgressQueueCeiling = 50;

        // Which artefact naming the tables expand to. One constant rather than an
        // #ifdef at each use, so the two call sites cannot disagree.
        constexpr bool kWindowsArtifacts =
#ifdef _WIN32
            true;
#else
            false;
#endif

        // Grace added to the child's own deadline beyond the caller's budget, so
        // build-lock.ps1's timeout fires FIRST and we report its message (which
        // names the holder) instead of a blunt "the child was killed".
        constexpr auto kChildDeadlineGrace = std::chrono::seconds(90);

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

        [[nodiscard]] std::string FormatUtc(std::chrono::system_clock::time_point when)
        {
            const std::time_t epoch = std::chrono::system_clock::to_time_t(when);
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

        // Stat one artefact. `buildStart` is the instant the build command was
        // launched: the difference between it and the artefact's mtime is what
        // separates "this build produced it" from "it was already there", and
        // that comparison is the whole anti-stale mechanism.
        [[nodiscard]] ArtifactState StatArtifact(const fs::path& absolute, const std::string& reportedPath,
                                                 std::chrono::system_clock::time_point buildStart)
        {
            ArtifactState state;
            state.Path = reportedPath;

            std::error_code ec;
            if (!fs::is_regular_file(absolute, ec))
                return state;
            state.Exists = true;
            state.SizeBytes = static_cast<u64>(fs::file_size(absolute, ec));
            if (ec)
                state.SizeBytes = 0;

            const auto written = fs::last_write_time(absolute, ec);
            if (ec)
                return state;
            const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(written);
            state.ModifiedUtc = FormatUtc(systemTime);
            state.MtimeKnown = true;
            state.AgeRelativeToStartSeconds = std::chrono::duration<f64>(systemTime - buildStart).count();
            return state;
        }

        // Walk up from the working directory looking for the lock script.
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
                if (fs::exists(cursor / kLockScriptRelative, ec))
                {
                    outRoot = cursor;
                    return true;
                }
                const fs::path parent = cursor.parent_path();
                if (parent.empty() || parent == cursor)
                    break;
                cursor = parent;
            }
            outError = std::string("Could not locate ") + kLockScriptRelative +
                       " in any parent of the working directory (looked in " + searched +
                       "). Every build in this repo goes through that script, so there is no build to start without "
                       "it — this command will not fall back to an unlocked cmake.";
            return false;
        }

        // The directory every worktree of this repo shares — where the build lock
        // lives. A worktree's `.git` is a FILE holding `gitdir: <path>`; the
        // common dir is that path's `commondir` if it has one, else itself.
        // Resolved by reading rather than by shelling out to git: the answer is a
        // path, and spawning a process to learn it would be a second, slower way
        // to be wrong.
        [[nodiscard]] bool FindGitCommonDir(const fs::path& repoRoot, fs::path& outDir)
        {
            std::error_code ec;
            const fs::path dotGit = repoRoot / ".git";
            if (fs::is_directory(dotGit, ec))
            {
                outDir = dotGit;
                return true;
            }
            if (!fs::is_regular_file(dotGit, ec))
                return false;

            bool ok = false;
            const std::string text = ReadWholeFile(dotGit, ok);
            constexpr std::string_view kPrefix = "gitdir:";
            if (!ok || !std::string_view(text).starts_with(kPrefix))
                return false;
            std::string target = text.substr(kPrefix.size());
            while (!target.empty() && (target.back() == '\n' || target.back() == '\r' || target.back() == ' '))
                target.pop_back();
            while (!target.empty() && (target.front() == ' ' || target.front() == '\t'))
                target.erase(target.begin());
            if (target.empty())
                return false;

            fs::path worktreeGitDir(target);
            if (worktreeGitDir.is_relative())
                worktreeGitDir = repoRoot / worktreeGitDir;

            // A linked worktree's gitdir holds a `commondir` pointing at the real
            // one; the main checkout's does not.
            const fs::path commonMarker = worktreeGitDir / "commondir";
            if (fs::is_regular_file(commonMarker, ec))
            {
                bool commonOk = false;
                std::string common = ReadWholeFile(commonMarker, commonOk);
                while (!common.empty() && (common.back() == '\n' || common.back() == '\r' || common.back() == ' '))
                    common.pop_back();
                if (commonOk && !common.empty())
                {
                    fs::path resolved(common);
                    if (resolved.is_relative())
                        resolved = worktreeGitDir / resolved;
                    outDir = fs::weakly_canonical(resolved, ec);
                    if (ec)
                        outDir = resolved;
                    return true;
                }
            }
            outDir = worktreeGitDir;
            return true;
        }

        // Is a lock slot free right now? Probe with the same exclusive open the
        // script's own acquire uses, then close it at once. A file's EXISTENCE
        // says nothing — every slot file outlives its holder — so the handle is
        // the only honest answer.
        //
        // Inherently a snapshot: someone can take the slot a microsecond later.
        // It is reported, never acted on — olo_build_run always goes through the
        // real acquire.
        // std::nullopt means NOT MEASURED, which is not the same as free — a
        // command that reported an unmeasured slot as available would be telling
        // a caller something it never checked.
        [[nodiscard]] std::optional<bool> IsSlotFree(const fs::path& slot)
        {
#ifdef _WIN32
            std::error_code ec;
            if (!fs::exists(slot, ec))
                return true; // never held since the last reboot
            const std::wstring wide = slot.wstring();
            const HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(handle);
                return true;
            }
            // ONLY a sharing/lock violation means somebody holds it. Every other
            // failure — an ACL that denies us write, the file vanishing between
            // the exists() check and here — is a probe that did not measure
            // anything, and reporting it as "held" would be the same conflation
            // the POSIX branch below refuses to make.
            const DWORD error = ::GetLastError();
            if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
                return false;
            return std::nullopt;
#else
            // The script's ownership is a Windows exclusive file handle; there is
            // no POSIX probe with the same meaning, and this command has no POSIX
            // caller today. Unknown, said plainly.
            (void)slot;
            return std::nullopt;
#endif
        }

        // ---- the PowerShell host ------------------------------------------

        [[nodiscard]] bool ResolvePowerShell(fs::path& outPath, std::string& outError)
        {
#ifdef _WIN32
            for (const wchar_t* candidate : { L"pwsh.exe", L"powershell.exe" })
            {
                wchar_t resolved[MAX_PATH]{};
                const DWORD length = ::SearchPathW(nullptr, candidate, nullptr, MAX_PATH, resolved, nullptr);
                if (length > 0 && length < MAX_PATH)
                {
                    outPath = fs::path(resolved);
                    return true;
                }
            }
            outError = "Neither pwsh.exe nor powershell.exe is on PATH, so build-lock.ps1 cannot be run. "
                       "Install PowerShell 7 (the repo's documented prerequisite) or build from a shell.";
            return false;
#else
            for (const char* candidate : { "/usr/bin/pwsh", "/usr/local/bin/pwsh", "/snap/bin/pwsh" })
            {
                std::error_code ec;
                if (fs::is_regular_file(candidate, ec))
                {
                    outPath = fs::path(candidate);
                    return true;
                }
            }
            outError = "pwsh was not found, so build-lock.ps1 cannot be run.";
            return false;
#endif
        }

        // ---- the child process ---------------------------------------------

        enum class ChildOutcome : u8
        {
            Exited = 0,  // ran to completion; ExitCode is meaningful
            SpawnFailed, // never started
            TimedOut,    // killed at our deadline
            Cancelled,   // killed because the caller cancelled the call
            // The OS wait on the child FAILED. Distinct from a timeout because
            // nothing about the build is known, including whether it is still
            // running — so the tree is killed and this is reported, never
            // reported as a timeout it was not.
            MonitorFailed,
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
                case ChildOutcome::MonitorFailed:
                    return "monitor-failed";
            }
            return "unknown";
        }

        struct ChildResult
        {
            ChildOutcome Outcome = ChildOutcome::SpawnFailed;
            int ExitCode = -1;
            std::string Error;
            std::string Console;

            [[nodiscard]] bool Completed() const
            {
                return Outcome == ChildOutcome::Exited;
            }
        };

        // Emit progress from the growing console log, and answer whether the
        // call has been cancelled. Shared by both platform branches so the two
        // cannot drift on when a build is abandoned.
        struct RunWatch
        {
            IAutomationHost* Host = nullptr;
            fs::path ConsolePath;
            std::chrono::steady_clock::time_point Started{};
            std::chrono::seconds LockBudget{ 0 };
            std::string Target;

            [[nodiscard]] bool Tick()
            {
                if (Host == nullptr)
                    return false;
                ReportProgress();
                return Host->IsCurrentCallCancelled();
            }

            // When the transcript first showed the lock acquired, i.e. where the
            // wall time stops being a queue wait and starts being a build.
            // Unset when the child finished between two polls, or never acquired.
            [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> AcquiredAt() const
            {
                return m_AcquiredAt;
            }

          private:
            // Re-reads the WHOLE log each tick, unlike the test runner's
            // append-only scan. A build log is a few hundred KB where a
            // 7000-case test run is tens of megabytes, and the two signals here
            // (the last ninja edge, and whether the lock has been acquired yet)
            // are both "state at the end", not counts to accumulate.
            void ReportProgress()
            {
                bool ok = false;
                const std::string text = ReadWholeFile(ConsolePath, ok);
                if (!ok)
                    return;

                const LockTranscript lock = ReadLockTranscript(text);
                if (!m_AcquiredAt.has_value() && lock.Outcome == LockOutcome::Acquired)
                    m_AcquiredAt = std::chrono::steady_clock::now();

                i64 permille = 0;
                std::string message;

                if (lock.Outcome != LockOutcome::Acquired)
                {
                    // Still queued. The bar creeps across the first 5% with the
                    // wait, which is honest about it being a wait rather than
                    // pretending nothing is happening.
                    const f64 waited = std::chrono::duration<f64>(std::chrono::steady_clock::now() - Started).count();
                    const f64 budget = std::max<f64>(1.0, static_cast<f64>(LockBudget.count()));
                    permille = static_cast<i64>(std::min(1.0, waited / budget) * static_cast<f64>(kProgressQueueCeiling));
                    message = "queued for the build lock";
                    if (lock.HeldByPid > 0)
                    {
                        message += " (held by pid " + std::to_string(lock.HeldByPid);
                        if (!lock.HeldByWorktree.empty())
                            message += " in " + lock.HeldByWorktree;
                        message += ")";
                    }
                }
                else if (const BuildProgress build = ReadLastNinjaProgress(text); build.Known())
                {
                    permille = kProgressQueueCeiling +
                               static_cast<i64>((static_cast<f64>(build.Done) / static_cast<f64>(build.Total)) *
                                                static_cast<f64>(kProgressTotal - kProgressQueueCeiling));
                    message = "building " + Target + " — " + std::to_string(build.Done) + "/" +
                              std::to_string(build.Total) + " steps";
                }
                else
                {
                    // Acquired, but nothing countable yet — the MSVC generator
                    // prints no edge counter at all, so this is its steady state
                    // rather than a transient. Report the phase, not a fraction.
                    permille = kProgressQueueCeiling;
                    message = "building " + Target;
                }

                // Clamped monotonic: EmitProgress requires a non-decreasing
                // value per call, and ninja's total can be revised downward
                // mid-build when edges turn out to be up to date.
                permille = std::clamp(permille, m_LastPermille, kProgressTotal);
                if (m_EverEmitted && permille == m_LastPermille && message == m_LastMessage)
                    return; // nothing new to say; EmitProgress must not go backwards or repeat

                m_LastPermille = permille;
                m_LastMessage = message;
                m_EverEmitted = true;
                Host->EmitProgress(static_cast<f64>(permille), static_cast<f64>(kProgressTotal), message);
            }

            i64 m_LastPermille = 0;
            std::string m_LastMessage;
            bool m_EverEmitted = false;
            // Observed by the same poll that reports progress — there is no
            // second place the acquire becomes visible.
            std::optional<std::chrono::steady_clock::time_point> m_AcquiredAt;
        };

#ifdef _WIN32
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

        // Owns the job object the build tree lives in. Closing it kills every
        // process still inside — which is the point: see contract note 3.
        class KillOnCloseJob
        {
          public:
            KillOnCloseJob()
            {
                m_Handle = ::CreateJobObjectW(nullptr, nullptr);
                if (m_Handle == nullptr)
                    return;
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!::SetInformationJobObject(m_Handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                {
                    ::CloseHandle(m_Handle);
                    m_Handle = nullptr;
                }
            }
            ~KillOnCloseJob()
            {
                if (m_Handle != nullptr)
                    ::CloseHandle(m_Handle);
            }

            KillOnCloseJob(const KillOnCloseJob&) = delete;
            KillOnCloseJob& operator=(const KillOnCloseJob&) = delete;
            KillOnCloseJob(KillOnCloseJob&&) = delete;
            KillOnCloseJob& operator=(KillOnCloseJob&&) = delete;

            [[nodiscard]] bool Valid() const
            {
                return m_Handle != nullptr;
            }
            [[nodiscard]] HANDLE Get() const
            {
                return m_Handle;
            }
            void KillTree() const
            {
                if (m_Handle != nullptr)
                    ::TerminateJobObject(m_Handle, 1);
            }

          private:
            HANDLE m_Handle = nullptr;
        };

        [[nodiscard]] ChildResult RunChild(const fs::path& exe, const std::wstring& commandLine,
                                           const fs::path& workingDirectory, const fs::path& consolePath,
                                           std::chrono::milliseconds timeout, RunWatch& watch)
        {
            ChildResult result;

            const KillOnCloseJob job;
            if (!job.Valid())
            {
                // Refused rather than run unjobbed. Without the job a cancelled
                // build leaves ninja running with the lock already released,
                // which is the failure mode the lock exists to prevent — and a
                // build that quietly ran without its containment would be
                // indistinguishable from one that had it.
                result.Error = "Could not create the job object that contains the build (error " +
                               std::to_string(::GetLastError()) +
                               "). Refusing to start a build that cancellation could not fully stop.";
                return result;
            }

            SECURITY_ATTRIBUTES inheritable{};
            inheritable.nLength = sizeof(inheritable);
            inheritable.bInheritHandle = TRUE;

            // stdout and stderr both go to one file rather than a pipe: a pipe
            // nobody drains fills its buffer and BLOCKS the child, and a chatty
            // build would hang in a way that looks exactly like a slow compile.
            // FILE_SHARE_READ is what lets the progress watch read the log while
            // the child is still writing it.
            // .wstring(), never Widen(.string()): path::string() encodes through
            // the ANSI code page on MSVC, so a non-ASCII %TEMP% would be
            // mis-decoded as UTF-8 and every build would fail before starting.
            // The wide form is what the path already holds.
            const HANDLE console = ::CreateFileW(consolePath.wstring().c_str(), GENERIC_WRITE,
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

            std::wstring mutableCommandLine = commandLine;
            const std::wstring directory = workingDirectory.wstring();

            // SUSPENDED, then assigned to the job, then resumed. Starting it
            // running and assigning afterwards leaves a window in which the
            // child could spawn ninja outside the job, and those grandchildren
            // would then survive a cancellation.
            PROCESS_INFORMATION process{};
            if (!::CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                  directory.empty() ? nullptr : directory.c_str(), &startup, &process))
            {
                const DWORD error = ::GetLastError();
                ::CloseHandle(console);
                result.Error = "Could not start " + exe.generic_string() + " (CreateProcess error " +
                               std::to_string(error) + ").";
                return result;
            }
            ::CloseHandle(console); // our copy; the child holds its own

            if (!::AssignProcessToJobObject(job.Get(), process.hProcess))
            {
                const DWORD error = ::GetLastError();
                ::TerminateProcess(process.hProcess, 1);
                ::CloseHandle(process.hThread);
                ::CloseHandle(process.hProcess);
                result.Error = "Could not put the build into its job object (error " + std::to_string(error) +
                               "). The build was killed before it started rather than run uncontained.";
                return result;
            }
            ::ResumeThread(process.hThread);

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            result.Outcome = ChildOutcome::Exited;
            for (;;)
            {
                const DWORD waited = ::WaitForSingleObject(process.hProcess, static_cast<DWORD>(kPollInterval.count()));
                if (waited == WAIT_OBJECT_0)
                    break;
                // WAIT_FAILED would otherwise fall through forever: the wait IS
                // this loop's sleep, so a failing wait turns it into a busy spin
                // that re-reads the console log as fast as it can until the
                // deadline — up to four hours of a saturated core next to a
                // build. Kill the tree and say what happened.
                if (waited == WAIT_FAILED)
                {
                    const DWORD error = ::GetLastError();
                    result.Outcome = ChildOutcome::MonitorFailed;
                    result.Error = "Waiting on the build process failed (error " + std::to_string(error) +
                                   "), so its state is unknown; the whole build tree was killed rather than left "
                                   "running unwatched.";
                    job.KillTree();
                    ::WaitForSingleObject(process.hProcess, 30000);
                    break;
                }
                if (watch.Tick())
                {
                    result.Outcome = ChildOutcome::Cancelled;
                    job.KillTree();
                    ::WaitForSingleObject(process.hProcess, 30000);
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    result.Outcome = ChildOutcome::TimedOut;
                    job.KillTree();
                    ::WaitForSingleObject(process.hProcess, 30000);
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
                // Its own process GROUP, so cancelling kills the build tree and
                // not merely the shell in front of it — the POSIX half of
                // contract note 3.
                (void)::setpgid(0, 0);
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
            // Also in the parent: whichever runs first wins, and neither losing
            // the race leaves the child outside its group.
            (void)::setpgid(pid, pid);

            const auto killTree = [pid]()
            {
                if (::killpg(pid, SIGKILL) != 0)
                    (void)::kill(pid, SIGKILL);
            };

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
                    killTree();
                    (void)::waitpid(pid, &status, 0);
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    result.Outcome = ChildOutcome::TimedOut;
                    killTree();
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

        // A scratch directory for one invocation's console log, removed when the
        // invocation ends however it ends.
        class ScratchDirectory
        {
          public:
            explicit ScratchDirectory(const char* stem)
            {
                std::error_code ec;
                const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                const fs::path candidate = fs::temp_directory_path(ec) /
                                           (std::string("olo-") + stem + "-" + std::to_string(static_cast<u64>(stamp)));
                if (ec)
                    return;
                // EXCLUSIVE creation: create_directory returns false without an
                // error when the directory already exists, and a pre-existing one
                // in a world-writable /tmp may have been planted (CWE-379).
                if (!fs::create_directory(candidate, ec) || ec)
                    return;
                m_Path = candidate;
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

        // ---- shared resolution ----------------------------------------------

        struct Session
        {
            fs::path RepoRoot;
            fs::path LockScript;
            fs::path PowerShell;
            std::string BuildDir;
            std::string Config;
        };

        [[nodiscard]] bool ResolveTree(const Json& args, Session& out, std::string& outError)
        {
            out.BuildDir = args.value("buildDir", std::string(kBuildDirs.front()));
            if (!IsKnownBuildDir(out.BuildDir))
            {
                std::string known;
                for (const std::string_view dir : kBuildDirs)
                {
                    if (!known.empty())
                        known += ", ";
                    known += std::string(dir);
                }
                outError = "Unknown 'buildDir' '" + out.BuildDir + "'. This command only builds the trees "
                                                                   "CMakePresets.json defines: " +
                           known +
                           ". (The name is interpolated into the command build-lock.ps1 executes, so it is "
                           "matched against that list rather than escaped.)";
                return false;
            }
            out.Config = args.value("config", std::string("Debug"));
            if (!IsKnownConfig(out.Config))
            {
                outError = "Unknown 'config' '" + out.Config + "'. Expected Debug, Release or Dist.";
                return false;
            }
            return true;
        }

        [[nodiscard]] bool OpenSession(const Json& args, Session& out, std::string& outError)
        {
            if (!FindRepoRoot(out.RepoRoot, outError))
                return false;
            out.LockScript = out.RepoRoot / kLockScriptRelative;
            return ResolveTree(args, out, outError);
        }

        // Everything that must be true before a build can even be attempted.
        // Each failure is a NAMED error, never an empty success — a caller that
        // read "0 errors" out of a build that never started would take it as
        // proof the tree compiles.
        [[nodiscard]] bool CheckBuildPreconditions(const Session& session, std::string& outError)
        {
            std::error_code ec;
            const fs::path treePath = session.RepoRoot / session.BuildDir;
            if (!fs::is_directory(treePath, ec))
            {
                outError = "The build tree '" + session.BuildDir + "' does not exist at " +
                           treePath.generic_string() + ". Configure it first: cmake --preset " +
                           (session.BuildDir == "build-cached" ? "dev-cached"
                                                               : (session.BuildDir == "build" ? "msvc" : "clangcl")) +
                           ". This command never configures — a configure is a different decision with different "
                           "costs, and doing it silently would hide it.";
                return false;
            }
            if (!fs::is_regular_file(treePath / "CMakeCache.txt", ec))
            {
                outError = "The build tree '" + session.BuildDir + "' has no CMakeCache.txt, so it is not "
                                                                   "configured. Run the matching cmake --preset first.";
                return false;
            }
            // Checked even though we never configure: `cmake --build` re-runs the
            // generator when the build system is out of date, and that hits the
            // repo's VCPKG_ROOT guard (issue #773). Better a named refusal here
            // than a cmake error deep in a build log.
            if (!Env::Get("VCPKG_ROOT").has_value())
            {
                outError = "VCPKG_ROOT is not set in this editor's environment (issue #773). `cmake --build` "
                           "re-runs the generator when the build system is stale, and that stops at the repo's "
                           "guard. Set VCPKG_ROOT and restart the editor — an editor launched before the variable "
                           "existed does not inherit it.";
                return false;
            }
            std::error_code lockEc;
            if (!fs::is_regular_file(session.LockScript, lockEc))
            {
                outError = "The build lock script is missing at " + session.LockScript.generic_string() +
                           ". Refusing to build outside the lock.";
                return false;
            }
            return true;
        }

        // ---- olo_build_list --------------------------------------------------

        AutomationResult Handle_BuildList(IAutomationHost& /*host*/, const Json& args)
        {
            Session session;
            if (std::string error; !OpenSession(args, session, error))
                return AutomationResult::Error(error);

            const auto now = std::chrono::system_clock::now();
            std::error_code ec;

            Json out;
            out["repoRoot"] = session.RepoRoot.generic_string();
            out["buildDir"] = session.BuildDir;
            out["config"] = session.Config;

            const fs::path treePath = session.RepoRoot / session.BuildDir;
            const bool configured = fs::is_regular_file(treePath / "CMakeCache.txt", ec);
            out["configured"] = configured;
            out["vcpkgRootSet"] = Env::Get("VCPKG_ROOT").has_value();

            // Advisory: whether a build could start right now without queueing.
            // Reported, never acted on — olo_build_run always goes through the
            // real acquire, where the answer is authoritative.
            Json lock;
            if (fs::path commonDir; FindGitCommonDir(session.RepoRoot, commonDir))
            {
                lock["directory"] = commonDir.generic_string();
                Json slots = Json::array();
                sizet free = 0;
                bool measured = true;
                for (const char* name : { "olo-build.lock", "olo-build.slot1.lock" })
                {
                    const std::optional<bool> slotFree = IsSlotFree(commonDir / name);
                    Json entry{ { "slot", name } };
                    if (slotFree.has_value())
                    {
                        entry["free"] = *slotFree;
                        free += *slotFree ? 1 : 0;
                    }
                    else
                    {
                        measured = false;
                    }
                    slots.push_back(std::move(entry));
                }
                lock["slots"] = std::move(slots);
                if (measured)
                    lock["freeSlots"] = free;
                lock["note"] =
                    measured ? "A snapshot, not a reservation: another worktree can take a slot a microsecond "
                               "later. olo_build_run always goes through build-lock.ps1 regardless."
                             : "Slot state is not measurable on this platform, so it is reported as absent rather "
                               "than as free. olo_build_run acquires through build-lock.ps1, where the answer is "
                               "authoritative.";
            }
            else
            {
                lock["note"] = "Could not resolve the shared git directory, so the lock state is unknown here. "
                               "That does not affect olo_build_run, which acquires through build-lock.ps1.";
            }
            out["lock"] = std::move(lock);

            Json targets = Json::array();
            for (const TargetSpec& spec : kTargets)
            {
                const std::string relative = ExpandArtifact(spec.Artifact, session.Config, kWindowsArtifacts);
                const fs::path absolute = spec.ArtifactUnderBuildDir ? treePath / relative : session.RepoRoot / relative;
                const std::string reported =
                    spec.ArtifactUnderBuildDir ? session.BuildDir + "/" + relative : relative;

                Json entry;
                entry["target"] = std::string(spec.Name);
                entry["buildable"] = spec.RefusedBecause.empty();
                if (!spec.RefusedBecause.empty())
                    entry["refusedBecause"] = std::string(spec.RefusedBecause);
                entry["description"] = std::string(spec.Description);
                // `now` as the reference instant, so `ageSeconds` reads as "how
                // old is this artefact" rather than as a build verdict.
                const ArtifactState state = StatArtifact(absolute, reported, now);
                entry["artifact"] = ArtifactJson(state);
                if (state.Exists)
                    entry["artifactAgeSeconds"] = -state.AgeRelativeToStartSeconds;
                targets.push_back(std::move(entry));
            }
            out["targets"] = std::move(targets);
            return AutomationResult::Structured(out);
        }

        // ---- olo_build_run ---------------------------------------------------

        // Resolve the caller's target list against the allow-list. Every refusal
        // names its mechanism; "no targets" is one of them, because a default
        // target would be `all`, which reaches OloEditor.
        [[nodiscard]] bool ResolveTargets(const Json& args, std::vector<const TargetSpec*>& out, std::string& outError)
        {
            if (!args.contains("targets") || !args["targets"].is_array() || args["targets"].empty())
            {
                outError = "Give a non-empty 'targets' array. There is deliberately no default: the default "
                           "target of a CMake build is 'all', which links OloEditor — the image this editor is "
                           "running from, which the linker cannot replace. Buildable targets: " +
                           BuildableTargetList() + ".";
                return false;
            }
            std::set<std::string> seen;
            for (const auto& entry : args["targets"])
            {
                if (!entry.is_string() || entry.get<std::string>().empty())
                {
                    outError = "Invalid entry in 'targets': expected non-empty CMake target names.";
                    return false;
                }
                const std::string name = entry.get<std::string>();
                const TargetSpec* spec = FindTarget(name);
                if (spec == nullptr)
                {
                    outError = "Unknown target '" + name +
                               "'. This command builds only the targets it has an artefact path and an "
                               "in-editor safety verdict for, and the name is interpolated into the command "
                               "build-lock.ps1 runs, so it is matched rather than escaped. Buildable: " +
                               BuildableTargetList() + ".";
                    return false;
                }
                if (!spec->RefusedBecause.empty())
                {
                    outError = "Refusing to build '" + name + "' from inside the editor. " +
                               std::string(spec->RefusedBecause);
                    return false;
                }
                if (!seen.insert(name).second)
                {
                    outError = "Target '" + name + "' is listed twice. Each target is built once, in order.";
                    return false;
                }
                out.push_back(spec);
            }
            return true;
        }

        AutomationResult Handle_BuildRun(IAutomationHost& host, const Json& args)
        {
            Session session;
            if (std::string error; !OpenSession(args, session, error))
                return AutomationResult::Error(error);

            std::vector<const TargetSpec*> targets;
            if (std::string error; !ResolveTargets(args, targets, error))
                return AutomationResult::Error(error);

            if (std::string error; !CheckBuildPreconditions(session, error))
                return AutomationResult::Error(error);

            if (std::string error; !ResolvePowerShell(session.PowerShell, error))
                return AutomationResult::Error(error);

            const auto lockWaitSeconds =
                std::clamp<long long>(args.value("lockWaitSeconds", 300LL), 0LL, 3600LL);
            const auto timeoutSeconds = std::clamp<long long>(args.value("timeoutSeconds", 3600LL), 60LL, 14400LL);
            const auto maxDiagnostics =
                static_cast<sizet>(std::clamp<long long>(args.value("maxDiagnostics", 100LL), 1LL, 1000LL));

            // -TimeoutMinutes is an int, and 0 is exactly the fail-fast the
            // refusal path needs: the deadline is already past when the first
            // acquisition attempt fails. Rounding UP for anything else means a
            // caller asking for 30s gets a minute rather than a surprise
            // fail-fast.
            const long long lockWaitMinutes = lockWaitSeconds == 0 ? 0 : (lockWaitSeconds + 59) / 60;

            Json out;
            out["repoRoot"] = session.RepoRoot.generic_string();
            out["buildDir"] = session.BuildDir;
            out["config"] = session.Config;
            out["lockScript"] = std::string(kLockScriptRelative);
            out["lockWaitSeconds"] = lockWaitSeconds;
            // The script's -TimeoutMinutes is integer minutes, so a sub-minute
            // request is rounded UP. Reporting only what was asked for would
            // misstate the budget a refusal was measured against.
            out["lockWaitAppliedSeconds"] = lockWaitMinutes * 60;

            Json requested = Json::array();
            for (const TargetSpec* spec : targets)
                requested.push_back(std::string(spec->Name));
            out["targets"] = requested;

            std::vector<TargetResult> results;
            const auto overallStart = std::chrono::steady_clock::now();
            bool aborted = false;
            std::string abortReason;
            std::string lastConsole;

            for (const TargetSpec* target : targets)
            {
                const TargetSpec& spec = *target;
                TargetResult result;
                result.Target = std::string(spec.Name);

                // Cancellation is only observed inside RunChild's poll loop, so
                // without this a caller who cancels BETWEEN targets — or before
                // the first child has started — would still have the next build
                // acquire the lock and run to completion. Checked here so a
                // cancelled call stops taking the machine.
                if (!aborted && host.IsCurrentCallCancelled())
                {
                    aborted = true;
                    abortReason = "Not attempted: the call was cancelled before this target started.";
                }

                if (aborted)
                {
                    // Never silently dropped: a target that was never attempted
                    // is reported as such, with the reason, rather than left out
                    // of an array the caller would read as complete.
                    result.Outcome = TargetOutcome::Skipped;
                    result.Note = abortReason;
                    results.push_back(std::move(result));
                    continue;
                }

                const std::string relative = ExpandArtifact(spec.Artifact, session.Config, kWindowsArtifacts);
                const fs::path treePath = session.RepoRoot / session.BuildDir;
                const fs::path absolute = spec.ArtifactUnderBuildDir ? treePath / relative : session.RepoRoot / relative;
                const std::string reported =
                    spec.ArtifactUnderBuildDir ? session.BuildDir + "/" + relative : relative;

                const ScratchDirectory scratch("build-run");
                if (!scratch.Valid())
                {
                    // Reported as this target's failure, not as an early return:
                    // an earlier target may already have rebuilt (and building
                    // OloEngine runs GenerateBindings, which moves the working
                    // tree), so throwing the collected results away would tell
                    // the caller nothing happened when something did.
                    result.Outcome = TargetOutcome::Failed;
                    result.Note = "Could not create a temporary directory for the build log, so this target was "
                                  "never started.";
                    aborted = true;
                    abortReason = "Not attempted: '" + result.Target + "' could not be given a scratch directory.";
                    results.push_back(std::move(result));
                    continue;
                }
                const fs::path consolePath = scratch.File("build.log");

                // The instant the build starts, which every artefact mtime is
                // compared against. Taken BEFORE the before-stat so a file
                // written during the stat cannot read as pre-existing.
                const auto buildStart = std::chrono::system_clock::now();
                result.Before = StatArtifact(absolute, reported, buildStart);

                const std::string buildCommand = BuildCommand(session.BuildDir, spec.Name, session.Config);
                result.Command = buildCommand;

                RunWatch watch;
                watch.Host = &host;
                watch.ConsolePath = consolePath;
                watch.Started = std::chrono::steady_clock::now();
                watch.LockBudget = std::chrono::seconds(lockWaitMinutes * 60);
                watch.Target = result.Target;

                // Our deadline is deliberately LATER than the lock's own, so a
                // lock timeout is reported by the script (naming the holder)
                // rather than by us as an anonymous kill.
                const auto childTimeout =
                    std::chrono::seconds(lockWaitMinutes * 60 + timeoutSeconds) + kChildDeadlineGrace;

                const auto started = std::chrono::steady_clock::now();
#ifdef _WIN32
                // Quoted only where a value can contain a space: the script path
                // and the build command. Target, config and build dir came from
                // fixed tables, so they carry nothing that needs quoting — which
                // is the point of the tables.
                // Assembled from .wstring() parts for the same reason as the
                // console handle above: the two PATHS must not round-trip
                // through the ANSI code page. Only `buildCommand` is widened,
                // and it is ASCII by construction (three fixed vocabularies).
                const std::wstring commandLine = L"\"" + session.PowerShell.wstring() + L"\" -NoProfile -File \"" +
                                                 session.LockScript.wstring() + L"\" -Command \"" +
                                                 Widen(buildCommand) + L"\" -TimeoutMinutes " +
                                                 Widen(std::to_string(lockWaitMinutes));
                const ChildResult child =
                    RunChild(session.PowerShell, commandLine, session.RepoRoot, consolePath, childTimeout, watch);
#else
                const std::vector<std::string> arguments{ "-NoProfile", "-File",
                                                          session.LockScript.string(), "-Command",
                                                          buildCommand, "-TimeoutMinutes",
                                                          std::to_string(lockWaitMinutes) };
                const ChildResult child =
                    RunChild(session.PowerShell, arguments, session.RepoRoot, consolePath, childTimeout, watch);
#endif
                const auto finished = std::chrono::steady_clock::now();
                result.WallSeconds = std::chrono::duration<f64>(finished - started).count();
                // Reported only when the lock was actually acquired. The watch
                // latches the acquire on a poll, but a LATER transcript line can
                // still resolve the run to Superseded (nothing ran) — and a
                // build duration for a build that did not run is a false
                // measurement. An ORPHANED run keeps its split on purpose: the
                // build genuinely ran for that long before the lock killed it.
                if (const auto acquiredAt = watch.AcquiredAt();
                    acquiredAt.has_value() && (result.Lock.Outcome == LockOutcome::Acquired ||
                                               result.Lock.Outcome == LockOutcome::Orphaned))
                {
                    result.QueuedSeconds = std::chrono::duration<f64>(*acquiredAt - started).count();
                    result.BuildSeconds = std::chrono::duration<f64>(finished - *acquiredAt).count();
                }
                result.ExitCode = child.ExitCode;
                result.Lock = ReadLockTranscript(child.Console);
                result.Diagnostics =
                    ParseDiagnostics(child.Console, session.RepoRoot.generic_string(), session.BuildDir);
                lastConsole = child.Console;

                if (!child.Completed())
                {
                    // A child that never finished tells us nothing about the
                    // tree, so it is a failure with the reason named — including
                    // for cancellation, which is not a partial success.
                    result.Outcome = TargetOutcome::Failed;
                    result.Note = std::string("The build ") + ChildOutcomeName(child.Outcome) + ". " + child.Error +
                                  (child.Outcome == ChildOutcome::Cancelled
                                       ? " The whole build tree (pwsh, cmake, ninja and every compiler) was killed "
                                         "with its job object, so the lock was released after the build stopped, "
                                         "not before."
                                       : "");
                    result.After = StatArtifact(absolute, reported, buildStart);
                    aborted = true;
                    abortReason = "Not attempted: '" + result.Target + "' " + ChildOutcomeName(child.Outcome) + ".";
                    results.push_back(std::move(result));
                    continue;
                }

                result.After = StatArtifact(absolute, reported, buildStart);
                result.Outcome = Verdict(result.Lock.Outcome, child.ExitCode, result.After);

                switch (result.Outcome)
                {
                    case TargetOutcome::NotBuilt:
                        result.Note =
                            result.Lock.Outcome == LockOutcome::Superseded
                                ? "build-lock.ps1 STOOD DOWN: an identical build for this worktree was queued "
                                  "later, so this one exited 0 without building anything. The exit code is not a "
                                  "verdict here — nothing ran."
                                : "build-lock.ps1 never acquired the lock, so no build ran. Its own output "
                                  "(see 'lock.transcript') says why: normally the " +
                                      std::to_string(lockWaitSeconds) +
                                      "s wait budget expired while another worktree held it. Raise "
                                      "'lockWaitSeconds', or wait for the holder named there and in 'consoleTail'.";
                        break;
                    case TargetOutcome::MissingArtifact:
                        result.Note = "The build reported success (exit 0) but there is no artefact at " + reported +
                                      ". Exit codes, mtimes and empty logs can all fake a build; the artefact is "
                                      "the evidence, and it is not there.";
                        break;
                    case TargetOutcome::ArtifactUnverifiable:
                        result.Note = "The build reported success and " + reported +
                                      " exists, but its modification time could not be read, so whether THIS build "
                                      "produced it is unknown. Reported as unverified rather than as a pass: a "
                                      "measurement that did not happen is not evidence.";
                        break;
                    case TargetOutcome::UpToDate:
                        result.Note = "Nothing to do: the artefact was already current and this build did not "
                                      "replace it. Its timestamp is from an EARLIER build — check "
                                      "'artifact.modifiedUtc' before treating it as containing your change.";
                        break;
                    case TargetOutcome::Failed:
                        result.Note = result.Lock.Outcome == LockOutcome::Orphaned
                                          ? "The lock's parent watch killed this build: the editor process that "
                                            "launched it was gone. An orphaned build never succeeded, whatever the "
                                            "kill reported."
                                          : "The build failed. See 'diagnostics' for the records and 'consoleTail' "
                                            "for the text around them.";
                        break;
                    case TargetOutcome::Rebuilt:
                    case TargetOutcome::Skipped:
                        break;
                }

                if (!IsSuccess(result.Outcome))
                {
                    aborted = true;
                    abortReason = "Not attempted: '" + result.Target + "' did not succeed (" +
                                  TargetOutcomeName(result.Outcome) + "), so the rest of the list was not built.";
                }
                results.push_back(std::move(result));
            }

            const f64 wallSeconds =
                std::chrono::duration<f64>(std::chrono::steady_clock::now() - overallStart).count();

            sizet errors = 0;
            sizet warnings = 0;
            sizet rebuilt = 0;
            bool allSucceeded = true;
            Json perTarget = Json::array();
            for (const TargetResult& result : results)
            {
                const DiagnosticCounts counts = CountDiagnostics(result.Diagnostics);
                errors += counts.Errors;
                warnings += counts.Warnings;
                rebuilt += result.Outcome == TargetOutcome::Rebuilt ? 1 : 0;
                allSucceeded = allSucceeded && IsSuccess(result.Outcome);
                perTarget.push_back(TargetResultJson(result, maxDiagnostics));
            }

            out["results"] = std::move(perTarget);
            out["wallSeconds"] = wallSeconds;
            out["errorCount"] = errors;
            out["warningCount"] = warnings;
            out["rebuilt"] = rebuilt;
            // Only ever true when EVERY requested target reached a success
            // outcome AND its artefact is on disk. A caller may check this one
            // field instead of re-deriving the invariant, which is the same
            // contract olo_tests_run's `complete` carries.
            out["ok"] = allSucceeded;

            if (!allSucceeded)
            {
                out["consoleTail"] = TailOf(lastConsole, kConsoleTailChars);
                std::string summary;
                for (const TargetResult& result : results)
                {
                    if (IsSuccess(result.Outcome))
                        continue;
                    if (!summary.empty())
                        summary += " ";
                    summary += result.Target + ": " + TargetOutcomeName(result.Outcome) + ".";
                    if (!result.Note.empty())
                        summary += " " + result.Note;
                }
                OLO_CORE_WARN("[Automation] olo_build_run failed after {:.1f}s: {}", wallSeconds, summary);
                return AutomationResult::Error("The build did not succeed. " + summary + "\n\n" + out.dump(2));
            }

            OLO_CORE_INFO("[Automation] olo_build_run: {} target(s), {} rebuilt, {} warning(s) in {:.1f}s ({}/{})",
                          results.size(), rebuilt, warnings, wallSeconds, session.BuildDir, session.Config);
            return AutomationResult::Structured(out);
        }

        // ---- schema fragments shared by both commands ------------------------

        [[nodiscard]] Schema::Node TreeProps(Schema::Node node)
        {
            return node
                // EnumFrom, not a restated literal: the value set IS the table in
                // AutomationBuildInvocation.h, and a hand-copied second spelling of
                // it goes stale silently (the schema gate would then reject a tree
                // the command happily builds).
                .Prop("buildDir", Schema::String()
                                      .EnumFrom(kBuildDirs)
                                      .Desc("Which CMakePresets.json tree to use. Defaults to build-cached — the "
                                            "compiler-cached Ninja tree, and the only kind the build lock will ever "
                                            "run concurrently with another."))
                .Prop("config",
                      Schema::String().EnumFrom(kConfigs).Desc("Build configuration (default Debug)."));
        }
    } // namespace

    void RegisterBuildCommands(AutomationRegistry& registry)
    {
        {
            AutomationCommand command;
            command.Name = "olo_build_list";
            command.Toolset = "build";
            command.Title = "List build targets";
            command.Annotations = Json{ { "readOnlyHint", true }, { "openWorldHint", false } };
            command.Undo = AutomationUndo::None;
            command.Description =
                "Enumerate the CMake targets olo_build_run can build, each with its artefact path, size and "
                "build timestamp, plus whether the tree is configured and whether a build lock slot is free "
                "right now. Builds nothing. Use it to answer 'is the binary I am about to run older than my "
                "change?' — an exit code cannot tell a fresh artefact from last week's, and a timestamp can. "
                "Targets the editor holds open (OloEditor itself, OloEngine-ScriptCore) are listed with "
                "buildable:false and the mechanism that refuses them.";
            command.InputSchema = TreeProps(Schema::Object()).NoAdditional();
            command.OutputSchema =
                Schema::Object()
                    .Prop("repoRoot", Schema::String())
                    .Prop("buildDir", Schema::String())
                    .Prop("config", Schema::String())
                    .Prop("configured", Schema::Bool().Desc("False when the tree has no CMakeCache.txt."))
                    .Prop("vcpkgRootSet", Schema::Bool().Desc("False means a build would stop at the repo's guard."))
                    .Prop("lock", Schema::Object().Desc("Build-lock slot state — advisory only, a snapshot."))
                    .Prop("targets", Schema::Array(Schema::Object()
                                                       .Prop("target", Schema::String())
                                                       .Prop("buildable", Schema::Bool())
                                                       .Prop("refusedBecause", Schema::String())
                                                       .Prop("description", Schema::String())
                                                       .Prop("artifact", Schema::Object())
                                                       .Prop("artifactAgeSeconds", Schema::Number())))
                    .Required({ "repoRoot", "buildDir", "config", "configured", "targets" });
            command.Handler = Handle_BuildList;
            registry.Register(std::move(command));
        }

        {
            AutomationCommand command;
            command.Name = "olo_build_run";
            command.Toolset = "build";
            command.Title = "Build targets";
            command.DualAudienceContent = true;
            // Mutating but NOT ProjectWrite: a build changes the build tree and
            // bin/, not anything the user is AUTHORING (no scene, no component,
            // no asset registry entry) — the same line olo_tests_run draws.
            // destructiveHint stays false: the outputs are generated artefacts a
            // rebuild reproduces, not user data.
            command.Annotations =
                Json{ { "readOnlyHint", false }, { "openWorldHint", false }, { "destructiveHint", false } };
            // A build cannot be taken back from inside this process.
            command.Undo = AutomationUndo::Irreversible;
            command.Description =
                "Build one or more CMake targets and return STRUCTURED per-target results: outcome, exit code, "
                "wall time, the artefact's path AND timestamp, and every compiler/linker diagnostic as a record "
                "with file, line, column and code — no console scraping. Runs through "
                ".claude/skills/run-oloengine/build-lock.ps1, always: the lock bounds concurrent builds across "
                "every worktree, sets the job count from free memory, and kills the build if this editor dies. "
                "A build that could not start is an ERROR naming the reason (lock still held after "
                "'lockWaitSeconds', tree not configured, VCPKG_ROOT unset) — never an empty success. So is a "
                "zero exit code with no artefact, and a lock stand-down, which exits 0 having built nothing; "
                "'up-to-date' is reported separately from 'rebuilt' because a stale artefact and a fresh one "
                "look identical from an exit code. Targets are built one at a time, in order, each with its own "
                "lock ticket, and the list stops at the first failure. OloEditor and OloEngine-ScriptCore are "
                "REFUSED: this editor has them open and the linker cannot replace them. Note that building "
                "OloEngine runs GenerateBindings, which can rewrite tracked generated sources.";
            command.InputSchema =
                TreeProps(Schema::Object())
                    .Prop("targets",
                          Schema::Array(Schema::String())
                              .Desc("CMake targets to build, in order. Required and non-empty: there is no default, "
                                    "because CMake's default target is 'all', which links the running editor. "
                                    "Allowed: OloEngine, OloEngine-Tests, OloRuntime, OloServer, oloctl, "
                                    "OloEngine-LuaScriptCore."))
                    .Prop("lockWaitSeconds",
                          Schema::Int().Min(0).Max(3600).Desc(
                              "How long to queue for the build lock before giving up (default 300). 0 fails fast "
                              "when the lock is held, naming the holder. The wait is a real FIFO ticket, so it "
                              "never jumps another worktree."))
                    .Prop("timeoutSeconds",
                          Schema::Int().Min(60).Max(14400).Desc(
                              "Kill the build after this long, on top of the lock wait (default 3600). A timeout "
                              "is an error, not a partial result, and the whole process tree is killed."))
                    .Prop("maxDiagnostics",
                          Schema::Int().Min(1).Max(1000).Desc(
                              "Cap on reported diagnostics per target (default 100). Errors are emitted before "
                              "warnings so a cap never hides the reason a build failed; any excess is counted in "
                              "'diagnosticsOmitted', never silently dropped."))
                    .NoAdditional();
            command.OutputSchema =
                Schema::Object()
                    .Prop("repoRoot", Schema::String())
                    .Prop("buildDir", Schema::String())
                    .Prop("config", Schema::String())
                    .Prop("targets", Schema::Array(Schema::String()).Desc("What was requested, in order."))
                    .Prop("ok", Schema::Bool().Desc("Always true on success: every requested target reached "
                                                    "'rebuilt' or 'up-to-date' AND its artefact is on disk."))
                    .Prop("rebuilt", Schema::Int().Min(0).Desc("Targets whose artefact this call actually replaced."))
                    .Prop("wallSeconds", Schema::Number())
                    .Prop("errorCount", Schema::Int().Min(0))
                    .Prop("warningCount", Schema::Int().Min(0))
                    .Prop("results",
                          Schema::Array(
                              Schema::Object()
                                  .Prop("target", Schema::String())
                                  .Prop("command", Schema::String().Desc(
                                                       "The cmake line the lock was asked to run, verbatim — paste "
                                                       "it into a shell to reproduce."))
                                  .Prop("outcome", Schema::String().Enum({ "rebuilt", "up-to-date", "failed",
                                                                           "missing-artifact",
                                                                           "artifact-unverifiable", "not-built",
                                                                           "skipped" }))
                                  .Prop("success", Schema::Bool())
                                  .Prop("exitCode", Schema::Int())
                                  .Prop("wallSeconds", Schema::Number())
                                  .Prop("queuedSeconds", Schema::Number().Desc(
                                                             "Of 'wallSeconds', how long was spent waiting for the "
                                                             "build lock. Omitted when the acquire was not observed."))
                                  .Prop("buildSeconds", Schema::Number().Desc(
                                                            "Of 'wallSeconds', how long the build itself took. A "
                                                            "large 'queuedSeconds' means the machine was busy, not "
                                                            "that the build is slow — opposite actions."))
                                  .Prop("artifact", Schema::Object().Desc(
                                                        "path, exists, sizeBytes and modifiedUtc AFTER the build — "
                                                        "the evidence an exit code cannot give."))
                                  .Prop("artifactBefore", Schema::Object().Desc("Present only when it moved."))
                                  .Prop("lock", Schema::Object().Desc(
                                                    "What build-lock.ps1 did: outcome, whether it was contended, "
                                                    "the job count it chose, who held it, and its verbatim "
                                                    "transcript."))
                                  .Prop("diagnostics",
                                        Schema::Array(Schema::Object()
                                                          .Prop("severity",
                                                                Schema::String().Enum({ "error", "warning", "note" }))
                                                          .Prop("file", Schema::String().Desc("Repo-relative."))
                                                          .Prop("line", Schema::Int().Min(1))
                                                          .Prop("column", Schema::Int().Min(1))
                                                          .Prop("code", Schema::String().Desc("C2065, LNK2019, ..."))
                                                          .Prop("message", Schema::String())
                                                          .Prop("occurrences",
                                                                Schema::Int().Min(2).Desc(
                                                                    "Present when one record came from several "
                                                                    "translation units."))))
                                  .Prop("diagnosticsOmitted", Schema::Int().Min(0))
                                  .Prop("errorCount", Schema::Int().Min(0))
                                  .Prop("warningCount", Schema::Int().Min(0))
                                  .Prop("note", Schema::String().Desc("Why, for every outcome that needs a "
                                                                      "sentence."))))
                    .Required({ "repoRoot", "buildDir", "config", "targets", "ok", "rebuilt", "wallSeconds",
                                "errorCount", "warningCount", "results" });
            command.Handler = Handle_BuildRun;
            registry.Register(std::move(command));
        }
    }
} // namespace OloEngine::Automation
