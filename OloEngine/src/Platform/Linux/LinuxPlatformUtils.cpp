#include "OloEnginePCH.h"
#include "OloEngine/Utils/PlatformUtils.h"

#ifdef OLO_PLATFORM_LINUX

#include <GLFW/glfw3.h>

#include <array>
#include <cstdlib> // getenv
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h> // waitpid, WIFEXITED, WEXITSTATUS
#include <unistd.h>   // fork, execvp, pipe, read, close, dup2, access, getcwd

namespace OloEngine
{
    f32 Time::GetTime()
    {
        if (HasMockTime())
            return GetMockTime();
        return static_cast<f32>(::glfwGetTime());
    }

    namespace
    {
        // One entry parsed from the Win32 commdlg filter string: a human-readable
        // description plus the file globs that belong to it.
        struct FileFilter
        {
            std::string Description;
            std::vector<std::string> Patterns;
        };

        enum class DialogBackend
        {
            None,
            Zenity,
            KDialog
        };

        // Our callers (EditorLayer and the editor panels) pass the Win32 GetOpenFileName
        // filter format verbatim: a double-NUL-terminated buffer of alternating
        // "description\0pattern\0" pairs, where a pattern field may list several globs
        // separated by ';'. For example:
        //     "OloEditor Scene (*.olo;*.scene)\0*.olo;*.scene\0All Files\0*.*\0\0"
        // Parse it into structured entries we can hand to zenity / kdialog.
        std::vector<FileFilter> ParseWin32Filter(const char* filter)
        {
            std::vector<FileFilter> result;
            if (filter == nullptr)
                return result;

            const char* cursor = filter;
            while (*cursor != '\0')
            {
                std::string description(cursor);
                cursor += description.size() + 1; // step past the NUL terminator

                // A well-formed filter always pairs a description with a pattern field.
                // A malformed (odd-count) buffer that ends here is simply ignored.
                if (*cursor == '\0')
                    break;

                std::string_view patterns(cursor);
                cursor += patterns.size() + 1;

                FileFilter entry;
                entry.Description = std::move(description);

                sizet start = 0;
                while (start <= patterns.size())
                {
                    const sizet sep = patterns.find(';', start);
                    const std::string_view glob = (sep == std::string_view::npos)
                                                      ? patterns.substr(start)
                                                      : patterns.substr(start, sep - start);
                    if (!glob.empty())
                    {
                        // The Windows "all files" idiom '*.*' only matches names that
                        // contain a dot on a Linux glob; translate it to a bare '*'.
                        entry.Patterns.emplace_back(glob == "*.*" ? "*" : std::string(glob));
                    }
                    if (sep == std::string_view::npos)
                        break;
                    start = sep + 1;
                }

                if (!entry.Description.empty() && !entry.Patterns.empty())
                    result.push_back(std::move(entry));
            }
            return result;
        }

        // Side-effect-free PATH lookup (unlike spawning `prog --version`): scan the
        // ':'-separated $PATH for an executable entry.
        bool IsExecutableInPath(const char* program)
        {
            const char* pathEnv = ::getenv("PATH");
            if (pathEnv == nullptr)
                return false;

            std::string_view path(pathEnv);
            sizet start = 0;
            while (start <= path.size())
            {
                const sizet sep = path.find(':', start);
                const std::string_view dir = (sep == std::string_view::npos)
                                                 ? path.substr(start)
                                                 : path.substr(start, sep - start);
                if (!dir.empty())
                {
                    std::string candidate(dir);
                    candidate += '/';
                    candidate += program;
                    if (::access(candidate.c_str(), X_OK) == 0)
                        return true;
                }
                if (sep == std::string_view::npos)
                    break;
                start = sep + 1;
            }
            return false;
        }

        DialogBackend SelectBackend()
        {
            const bool haveZenity = IsExecutableInPath("zenity");
            const bool haveKDialog = IsExecutableInPath("kdialog");
            if (!haveZenity && !haveKDialog)
                return DialogBackend::None;
            if (haveZenity && !haveKDialog)
                return DialogBackend::Zenity;
            if (haveKDialog && !haveZenity)
                return DialogBackend::KDialog;

            // Both present — prefer the one that matches the running desktop so the
            // dialog blends in with the rest of the session (KDE/Plasma -> kdialog).
            if (const char* desktop = ::getenv("XDG_CURRENT_DESKTOP"))
            {
                const std::string_view d(desktop);
                if (d.find("KDE") != std::string_view::npos || d.find("Plasma") != std::string_view::npos ||
                    d.find("plasma") != std::string_view::npos)
                {
                    return DialogBackend::KDialog;
                }
            }
            return DialogBackend::Zenity;
        }

        std::string CurrentWorkingDir()
        {
            std::array<char, 4096> buffer{};
            if (::getcwd(buffer.data(), buffer.size()) != nullptr)
                return std::string(buffer.data());
            return {};
        }

        // Spawn argv[0] (resolved via PATH) without a shell — no quoting/escaping
        // pitfalls — and capture its stdout. Returns true only when the child exits
        // with status 0. outSpawnFailed is set when the program could not be launched
        // at all (missing backend), which the caller distinguishes from a user cancel.
        bool RunCaptureStdout(const std::vector<std::string>& argv, std::string& outResult, bool& outSpawnFailed)
        {
            outResult.clear();
            outSpawnFailed = false;
            if (argv.empty())
            {
                outSpawnFailed = true;
                return false;
            }

            int pipeFds[2] = { -1, -1 };
            if (::pipe(pipeFds) != 0)
            {
                OLO_CORE_ERROR("FileDialogs: pipe() failed (errno={})", errno);
                outSpawnFailed = true;
                return false;
            }

            // Build the char* vector in the parent: after fork() in a multithreaded
            // process only async-signal-safe calls are allowed in the child, and
            // allocating here keeps the child path malloc-free.
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for (const std::string& arg : argv)
                cargv.push_back(const_cast<char*>(arg.c_str()));
            cargv.push_back(nullptr);

            const pid_t pid = ::fork();
            if (pid < 0)
            {
                OLO_CORE_ERROR("FileDialogs: fork() failed (errno={})", errno);
                ::close(pipeFds[0]);
                ::close(pipeFds[1]);
                outSpawnFailed = true;
                return false;
            }

            if (pid == 0)
            {
                // Child: redirect stdout to the pipe, then exec. Only async-signal-safe
                // calls below.
                ::close(pipeFds[0]);
                if (::dup2(pipeFds[1], STDOUT_FILENO) < 0)
                    _exit(127);
                ::close(pipeFds[1]);
                ::execvp(cargv[0], cargv.data());
                // Only reached if exec failed (e.g. binary not on PATH).
                _exit(127);
            }

            // Parent: drain the pipe until EOF.
            ::close(pipeFds[1]);
            std::array<char, 1024> buffer{};
            for (;;)
            {
                const ssize_t n = ::read(pipeFds[0], buffer.data(), buffer.size());
                if (n > 0)
                {
                    outResult.append(buffer.data(), static_cast<sizet>(n));
                }
                else if (n == 0)
                {
                    break;
                }
                else if (errno == EINTR)
                {
                    continue;
                }
                else
                {
                    break;
                }
            }
            ::close(pipeFds[0]);

            int status = 0;
            while (::waitpid(pid, &status, 0) == -1)
            {
                if (errno != EINTR)
                    break;
            }

            // zenity / kdialog terminate the selected path with a newline.
            while (!outResult.empty() && (outResult.back() == '\n' || outResult.back() == '\r'))
                outResult.pop_back();

            if (WIFEXITED(status))
            {
                const int code = WEXITSTATUS(status);
                if (code == 127)
                {
                    // Our own exec-failed sentinel — backend not actually runnable.
                    outSpawnFailed = true;
                    return false;
                }
                // 0 = OK, 1 = user cancelled (both zenity and kdialog).
                return code == 0;
            }
            return false;
        }

        // Fire-and-forget launch of argv[0] (resolved via PATH) with no shell — no
        // quoting/escaping pitfalls. Used to open the OS file manager without
        // blocking the editor UI thread. Double-forks so the grandchild that runs
        // the file manager is reparented to init and never lingers as a zombie.
        bool LaunchDetached(const std::vector<std::string>& argv)
        {
            if (argv.empty())
                return false;

            // Build the char* vector in the parent — after fork() only async-signal-safe
            // calls are allowed in the child, so keep its path malloc-free.
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for (const std::string& arg : argv)
                cargv.push_back(const_cast<char*>(arg.c_str()));
            cargv.push_back(nullptr);

            const pid_t pid = ::fork();
            if (pid < 0)
            {
                OLO_CORE_ERROR("ShowInFileManager: fork() failed (errno={})", errno);
                return false;
            }

            if (pid == 0)
            {
                // Intermediate child: fork again and exit immediately so the grandchild
                // is orphaned onto init (PID 1), which reaps it — the parent never has
                // to wait on the file manager itself.
                const pid_t grandchild = ::fork();
                if (grandchild == 0)
                {
                    ::execvp(cargv[0], cargv.data());
                    _exit(127); // only reached if exec failed
                }
                _exit(grandchild < 0 ? 127 : 0);
            }

            // Parent: reap the intermediate child, which exits right away.
            int status = 0;
            while (::waitpid(pid, &status, 0) == -1)
            {
                if (errno != EINTR)
                    break;
            }
            return true;
        }

        std::vector<std::string> BuildZenityArgs(const std::vector<FileFilter>& filters, const std::string& startDir, bool save)
        {
            std::vector<std::string> argv;
            argv.emplace_back("zenity");
            argv.emplace_back("--file-selection");
            if (save)
                argv.emplace_back("--save");
            argv.emplace_back(std::string("--title=") + (save ? "Save File" : "Open File"));

            if (!startDir.empty())
            {
                // A trailing slash makes zenity treat the value as a directory to open
                // in, rather than a suggested filename.
                std::string filename = startDir;
                if (filename.back() != '/')
                    filename += '/';
                argv.emplace_back("--filename=" + filename);
            }

            // Note: --confirm-overwrite is intentionally omitted. Modern zenity (GTK4)
            // removed the flag and rejects it, while still confirming overwrites by
            // default; passing it would break Save on those builds.

            for (const FileFilter& f : filters)
            {
                // zenity filter syntax: "Description | *.glob1 *.glob2"
                std::string spec = f.Description;
                spec += " |";
                for (const std::string& pattern : f.Patterns)
                {
                    spec += ' ';
                    spec += pattern;
                }
                argv.emplace_back("--file-filter=" + spec);
            }
            return argv;
        }

        std::vector<std::string> BuildKDialogArgs(const std::vector<FileFilter>& filters, const std::string& startDir, bool save)
        {
            std::vector<std::string> argv;
            argv.emplace_back("kdialog");
            argv.emplace_back("--title");
            argv.emplace_back(save ? "Save File" : "Open File");
            argv.emplace_back(save ? "--getsavefilename" : "--getopenfilename");
            argv.emplace_back(startDir.empty() ? std::string(".") : startDir);

            if (!filters.empty())
            {
                // kdialog filter syntax: "*.glob1 *.glob2|Description" entries joined
                // by newlines.
                std::string spec;
                for (sizet i = 0; i < filters.size(); ++i)
                {
                    if (i != 0)
                        spec += '\n';
                    for (sizet j = 0; j < filters[i].Patterns.size(); ++j)
                    {
                        if (j != 0)
                            spec += ' ';
                        spec += filters[i].Patterns[j];
                    }
                    spec += '|';
                    spec += filters[i].Description;
                }
                argv.emplace_back(std::move(spec));
            }
            return argv;
        }

        // Mirror the Win32 ofn.lpstrDefExt behaviour: if the user did not type an
        // extension, append the first concrete one from the filter (e.g. ".olo").
        void AppendDefaultExtension(std::string& path, const std::vector<FileFilter>& filters)
        {
            const sizet slash = path.find_last_of('/');
            const std::string_view base = (slash == std::string::npos)
                                              ? std::string_view(path)
                                              : std::string_view(path).substr(slash + 1);
            if (base.find('.') != std::string_view::npos)
                return; // user already specified an extension

            for (const FileFilter& f : filters)
            {
                for (const std::string& pattern : f.Patterns)
                {
                    // Accept only a single concrete "*.ext" glob (skip "*" and any
                    // pattern with an embedded wildcard after the dot).
                    if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.' &&
                        pattern.find('*', 2) == std::string::npos)
                    {
                        path += pattern.substr(1); // ".ext"
                        return;
                    }
                }
            }
        }

        // Shared driver for OpenFile / SaveFile. Returns the selected path, or an empty
        // string for both "user cancelled" and "no usable backend"; the two are
        // distinguished in the log (no caller inspects the return value beyond empty()).
        std::string RunFileDialog(const char* filter, const char* initialDir, bool save)
        {
            const DialogBackend backend = SelectBackend();
            if (backend == DialogBackend::None)
            {
                OLO_CORE_ERROR("FileDialogs: no native file-dialog backend found. Install 'zenity' "
                               "(GTK) or 'kdialog' (KDE) to enable {} dialogs; returning empty (treated "
                               "as cancel).",
                               save ? "save" : "open");
                return {};
            }

            const std::vector<FileFilter> filters = ParseWin32Filter(filter);
            const std::string startDir = (initialDir != nullptr) ? std::string(initialDir) : CurrentWorkingDir();

            const std::vector<std::string> argv = (backend == DialogBackend::Zenity)
                                                      ? BuildZenityArgs(filters, startDir, save)
                                                      : BuildKDialogArgs(filters, startDir, save);

            std::string result;
            bool spawnFailed = false;
            const bool ok = RunCaptureStdout(argv, result, spawnFailed);

            if (spawnFailed)
            {
                OLO_CORE_ERROR("FileDialogs: failed to launch '{}'; returning empty (treated as cancel).",
                               argv.empty() ? "?" : argv.front());
                return {};
            }
            if (!ok)
            {
                // Non-zero exit: the user cancelled (or the backend errored). Either way
                // no file was chosen, which matches the Windows "empty == cancelled" contract.
                return {};
            }

            if (save && !result.empty())
                AppendDefaultExtension(result, filters);

            return result;
        }
    } // namespace

    std::string FileDialogs::OpenFile(const char* const filter, const char* const initialDir)
    {
        return RunFileDialog(filter, initialDir, /*save=*/false);
    }

    std::string FileDialogs::SaveFile(const char* const filter, const char* const initialDir)
    {
        return RunFileDialog(filter, initialDir, /*save=*/true);
    }

    void FileDialogs::ShowInFileManager(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
        {
            OLO_CORE_WARN("ShowInFileManager: path does not exist: '{}'", path.string());
            return;
        }

        // xdg-open has no "select this file" mode, so when handed a file we open its
        // containing directory — which is what the caller wants (reveal where it lives).
        std::filesystem::path target = (std::filesystem::is_directory(path, ec) && !ec)
                                           ? path
                                           : path.parent_path();
        if (target.empty())
            target = std::filesystem::path(".");

        if (!IsExecutableInPath("xdg-open"))
        {
            OLO_CORE_ERROR("ShowInFileManager: 'xdg-open' not found on PATH; cannot reveal '{}'.", target.string());
            return;
        }

        if (!LaunchDetached({ "xdg-open", target.string() }))
            OLO_CORE_ERROR("ShowInFileManager: failed to launch xdg-open for '{}'.", target.string());
    }

} // namespace OloEngine

#endif // OLO_PLATFORM_LINUX
