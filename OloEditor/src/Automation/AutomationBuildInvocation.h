#pragma once

// Pure parsing, classification and freshness reasoning for the structured build
// command olo_build_run / olo_build_list (issue #1163, Epic H slice 3).
//
// Slice 1 (#1130) closed "run the thing that proves it, read a structured
// result". This closes the half before it: PRODUCE the thing. Until now an
// agent that wanted OloEngine-Tests built before running it had to leave the
// control plane entirely.
//
// THE INVARIANT THIS FILE EXISTS FOR: a build's exit code is not evidence that
// anything was built. Three separate mechanisms in this repo report a build that
// did not happen as exit 0:
//
//   1. `build-lock.ps1` STANDS DOWN with exit 0 when an identical command from
//      the same worktree was queued later (its Test-Superseded path). Nothing
//      ran; the caller sees success.
//   2. An incremental build with nothing to do exits 0 and touches no artefact.
//      That is legitimate — but it is indistinguishable from (1) and from (3)
//      unless the artefact is inspected.
//   3. A build that never started at all (no configured tree, no VCPKG_ROOT)
//      can still leave last week's binary sitting exactly where a caller looks.
//
// So the verdict is taken from the ARTEFACT, not the exit code: every target's
// primary output is stat'd before and after, and the result says whether it was
// replaced, was already up to date, or is missing. `Verdict()` below is the one
// place those three inputs are combined.
//
// Everything here is free functions over PODs with NO editor, process or
// filesystem dependency — only nlohmann::json and the stdlib — so it is unit
// tested headlessly (OloEngine/tests/MCP/McpAutomationBuildTest.cpp). The
// process spawning, the job object and the actual stat calls live in the
// handler (Automation/AutomationBuildCommands.cpp). Same split as
// MCP/McpTestExecution.h.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Automation::BuildInvocation
{
    using Json = nlohmann::json;

    // ---- the three fixed vocabularies ------------------------------------
    //
    // Target, configuration and build directory are INTERPOLATED into a
    // PowerShell command string that build-lock.ps1 writes to a script file and
    // executes. There is no escaping scheme that makes freehand text safe there,
    // so none is attempted: each of the three is resolved against a fixed table
    // and anything not in it is refused by name. That is the injection boundary,
    // and it is the same boundary that answers "may the editor build itself?".

    // Where a target's primary artefact lands, and whether it may be built from
    // inside a RUNNING editor.
    //
    // The layout is not guessed: `olo_configure_app` -> `olo_set_output_directories`
    // (cmake/CommonProperties.cmake) puts every app and the engine archive under
    // `<repo>/bin/<Config>/<target>/`, which is the SOURCE tree, not the build
    // tree — so two build trees write the same file and the path alone cannot
    // say which one produced it. OloEngine-Tests calls neither, so it keeps the
    // Ninja-multi-config default under the build directory.
    struct TargetSpec
    {
        std::string_view Name;
        // Path relative to the repo root (ArtifactUnderBuildDir false) or to the
        // build directory (true). `{CONFIG}` is substituted with the build
        // configuration; `{EXE}` with the platform executable suffix, `{LIB}`
        // with the static-library shape and `{DLL}` with the shared one.
        std::string_view Artifact;
        bool ArtifactUnderBuildDir = false;
        // Empty => buildable from inside the editor. Non-empty => refused, and
        // this is the sentence the caller gets.
        std::string_view RefusedBecause;
        std::string_view Description;
    };

    // Every target the root CMakeLists.txt defines that a caller might plausibly
    // ask for. A name that is not here is refused as unknown rather than passed
    // through — see the injection note above.
    inline constexpr std::array<TargetSpec, 8> kTargets{ {
        { "OloEngine", "bin/{CONFIG}/OloEngine/{LIB}OloEngine{LIBEXT}", false, "",
          "The engine static library. Depends on GenerateBindings, so building it can rewrite the "
          "tracked generated sources under OloEngine/src/Generated." },
        { "OloEngine-Tests", "OloEngine/tests/{CONFIG}/OloEngine-Tests{EXE}", true, "",
          "The GoogleTest binary olo_tests_run executes." },
        { "OloRuntime", "bin/{CONFIG}/OloRuntime/OloRuntime{EXE}", false, "",
          "The standalone game runtime. This is the binary BuildGamePanel copies into a packaged "
          "game, and the one its 'Build OloRuntime first' error asks for." },
        { "OloServer", "bin/{CONFIG}/OloServer/OloServer{EXE}", false, "", "The headless dedicated server." },
        { "oloctl", "bin/{CONFIG}/oloctl/oloctl{EXE}", false, "", "The automation CLI frontend (#1125)." },
        { "OloEngine-LuaScriptCore", "OloEditor/Resources/Scripts/{DLL}OloEngine-LuaScriptCore{DLLEXT}", false, "",
          "The Lua scripting shared library. The editor does not load it, so replacing it is safe "
          "while the editor runs." },
        // ---- refused, with the mechanism named --------------------------
        { "OloEditor", "bin/{CONFIG}/OloEditor/OloEditor{EXE}", false,
          "OloEditor is the image this process is running from, and Windows keeps a running image "
          "mapped — the linker cannot replace it. The build would compile everything and then fail "
          "at link with LNK1168. Build it from a shell, or close the editor first.",
          "The editor itself." },
        { "OloEngine-ScriptCore", "OloEditor/Resources/Scripts/OloEngine-ScriptCore.dll", false,
          "OloEngine-ScriptCore's assembly is loaded by Mono inside this running editor, so the C# "
          "build cannot overwrite it. Build it from a shell, or close the editor first.",
          "The C# scripting assembly." },
    } };

    [[nodiscard]] inline const TargetSpec* FindTarget(std::string_view name)
    {
        for (const TargetSpec& spec : kTargets)
        {
            if (spec.Name == name)
                return &spec;
        }
        return nullptr;
    }

    // Every target name, for an error message that tells the caller what it may
    // ask for instead of only what it may not.
    [[nodiscard]] inline std::string BuildableTargetList()
    {
        std::string list;
        for (const TargetSpec& spec : kTargets)
        {
            if (!spec.RefusedBecause.empty())
                continue;
            if (!list.empty())
                list += ", ";
            list += std::string(spec.Name);
        }
        return list;
    }

    // The build configurations CMakePresets.json declares. `Dist` is included
    // because it is a real configuration of every preset, not because anything
    // here is expected to ask for it.
    inline constexpr std::array<std::string_view, 3> kConfigs{ "Debug", "Release", "Dist" };

    [[nodiscard]] inline bool IsKnownConfig(std::string_view config)
    {
        return std::find(kConfigs.begin(), kConfigs.end(), config) != kConfigs.end();
    }

    // The build trees CMakePresets.json defines. `build-cached` is first because
    // it is CLAUDE.md's default and the only kind eligible for a second
    // concurrent slot under the lock; the other two are named so a caller
    // investigating a clang-cl or MSVC-specific failure can reach them.
    inline constexpr std::array<std::string_view, 3> kBuildDirs{ "build-cached", "build", "build-clang" };

    [[nodiscard]] inline bool IsKnownBuildDir(std::string_view dir)
    {
        return std::find(kBuildDirs.begin(), kBuildDirs.end(), dir) != kBuildDirs.end();
    }

    // Substitute the placeholders in a TargetSpec::Artifact. Kept here rather
    // than in the handler so the table and its expansion are tested together —
    // a wrong path makes a SUCCESSFUL build report "no artefact", which is the
    // safe direction but still a bug.
    [[nodiscard]] inline std::string ExpandArtifact(std::string_view pattern, std::string_view config, bool windows)
    {
        const auto replaceAll = [](std::string& text, std::string_view token, std::string_view with)
        {
            for (sizet at = text.find(token); at != std::string::npos; at = text.find(token, at + with.size()))
                text.replace(at, token.size(), with);
        };
        std::string out(pattern);
        replaceAll(out, "{CONFIG}", config);
        replaceAll(out, "{EXE}", windows ? ".exe" : "");
        // MSVC names a static archive `Foo.lib`; the GNU/Clang toolchains name it
        // `libFoo.a`. Same for a shared library: `Foo.dll` against `libFoo.so`.
        replaceAll(out, "{LIBEXT}", windows ? ".lib" : ".a");
        replaceAll(out, "{LIB}", windows ? "" : "lib");
        replaceAll(out, "{DLLEXT}", windows ? ".dll" : ".so");
        replaceAll(out, "{DLL}", windows ? "" : "lib");
        return out;
    }

    // ---- the command line -------------------------------------------------

    // The `cmake --build` command build-lock.ps1 is asked to run. One target per
    // invocation on purpose: `--target A --target B` builds both under one
    // `cmake` and there is then no honest way to attribute wall time — or a
    // failure — to one of them. Per-target results are the deliverable, so the
    // targets are built sequentially, each with its own lock ticket.
    //
    // `--parallel 6` is a HINT: the lock rewrites it from measured free memory
    // at acquire time, in either direction. It is passed anyway so that a run
    // with -Jobs -1 (verbatim) is still capped, never uncapped.
    [[nodiscard]] inline std::string BuildCommand(std::string_view buildDir, std::string_view target,
                                                  std::string_view config)
    {
        return "cmake --build " + std::string(buildDir) + " --target " + std::string(target) + " --config " +
               std::string(config) + " --parallel 6";
    }

    // ---- compiler diagnostics ---------------------------------------------

    enum class Severity : u8
    {
        Error = 0,
        Warning,
        Note,
    };

    [[nodiscard]] inline const char* SeverityName(Severity severity)
    {
        switch (severity)
        {
            case Severity::Error:
                return "error";
            case Severity::Warning:
                return "warning";
            case Severity::Note:
                return "note";
        }
        return "unknown";
    }

    // One compiler, linker or CMake diagnostic as a RECORD. The whole point of
    // the command: a caller reads `file`/`line`/`column`/`code`/`message`
    // instead of scraping console text and guessing at the shape.
    struct Diagnostic
    {
        Severity Level = Severity::Error;
        std::string File; // repo-relative where it could be made so; empty for a linker error
        int Line = 0;     // 0 when the diagnostic carries none
        int Column = 0;   // 0 when the toolchain emits none (MSVC's cl does not)
        std::string Code; // "C2065", "LNK2019", "C4996", ""; the toolchain's own id
        std::string Message;
        sizet Occurrences = 1; // >1 after Deduplicate(): the same record from N translation units

        [[nodiscard]] bool operator==(const Diagnostic& other) const
        {
            return Level == other.Level && File == other.File && Line == other.Line && Column == other.Column &&
                   Code == other.Code && Message == other.Message;
        }
    };

    namespace Detail
    {
        [[nodiscard]] inline std::string_view TrimAscii(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
                text.remove_suffix(1);
            return text;
        }

        // Strip a leading separator dash and the space around it. Handles both
        // the em dash build-lock.ps1's source carries (UTF-8 e2 80 94) and the
        // ASCII '-' a redirected PowerShell log actually contains — the host
        // transcodes to the console code page on the way out, so which one you
        // get depends on where you read it from, not on the script.
        [[nodiscard]] inline std::string_view TrimDashes(std::string_view text)
        {
            text = TrimAscii(text);
            constexpr std::string_view kEmDash = "\xe2\x80\x94";
            while (!text.empty())
            {
                if (text.front() == '-')
                    text.remove_prefix(1);
                else if (text.starts_with(kEmDash))
                    text.remove_prefix(kEmDash.size());
                else
                    break;
                text = TrimAscii(text);
            }
            return text;
        }

        // MSBuild appends the owning project to every diagnostic:
        // `... error C2065: 'x' [C:\repos\...\OloEngine.vcxproj]`. It is noise in
        // a per-file record, and leaving it in would make two otherwise identical
        // diagnostics from two projects fail to deduplicate.
        [[nodiscard]] inline std::string_view StripProjectSuffix(std::string_view message)
        {
            if (message.empty() || message.back() != ']')
                return message;
            const sizet open = message.rfind('[');
            if (open == std::string_view::npos)
                return message;
            const std::string_view inside = message.substr(open + 1, message.size() - open - 2);
            const bool isProject = inside.ends_with(".vcxproj") || inside.ends_with(".csproj") ||
                                   inside.ends_with(".metaproj") || inside.ends_with(".sln");
            return isProject ? TrimAscii(message.substr(0, open)) : message;
        }

        [[nodiscard]] inline bool IsDigits(std::string_view text)
        {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
                                                { return c >= '0' && c <= '9'; });
        }

        // Bounded so a pathological "12345678901234567890" cannot overflow; a
        // line number that large is not a line number.
        [[nodiscard]] inline int ToLineNumber(std::string_view text)
        {
            if (!IsDigits(text) || text.size() > 9)
                return 0;
            int value = 0;
            for (const char c : text)
                value = value * 10 + (c - '0');
            return value;
        }

        // Split a leading `<file>(<line>)` / `<file>(<line>,<col>)` prefix.
        // Returns false when the text does not start with one.
        // Split a leading `<file>(<line>)` / `<file>(<line>,<col>)` prefix, where
        // the closing paren is the one whose REMAINDER is a severity.
        //
        // Anchoring on the first ')' was wrong twice, and both are real lines:
        //
        //   C:\Program Files (x86)\Windows Kits\...\winnt.h(9): warning C4005: ...
        //       -- the first ')' closes "(x86)", "x86" is not a line number, and
        //          giving up there dropped the whole diagnostic;
        //   /h/f.cpp:99:5: error: no matching function for call to 'foo(3)'
        //       -- the first ')' closes "foo(3)" in the MESSAGE, which parses as
        //          a line number, so a gcc error was mis-split and then discarded.
        //
        // Requiring the remainder to be a severity settles both without a path
        // grammar: a parenthesis that is not a location prefix never has
        // ": error"/": warning" immediately after it. Candidates are scanned
        // LEFT to RIGHT so the FILE's parens are tried before the message's.
        [[nodiscard]] inline bool SplitSeverity(std::string_view rest, Severity& outLevel, std::string_view& outCode,
                                                std::string_view& outMessage);

        [[nodiscard]] inline bool SplitMsvcLocation(std::string_view text, std::string_view& outFile, int& outLine,
                                                    int& outColumn, std::string_view& outRest, Severity& outLevel,
                                                    std::string_view& outCode, std::string_view& outMessage)
        {
            for (sizet close = text.find(')'); close != std::string_view::npos; close = text.find(')', close + 1))
            {
                const sizet open = text.rfind('(', close);
                if (open == std::string_view::npos || open == 0)
                    continue;

                const std::string_view inside = text.substr(open + 1, close - open - 1);
                const sizet comma = inside.find(',');
                const std::string_view lineText = comma == std::string_view::npos ? inside : inside.substr(0, comma);
                const std::string_view columnText =
                    comma == std::string_view::npos ? std::string_view{} : inside.substr(comma + 1);
                const int line = ToLineNumber(lineText);
                if (line == 0)
                    continue;
                // A column that is present but unparseable means this was not a
                // location prefix at all (e.g. a call like `foo(a,b)`).
                if (!columnText.empty() && ToLineNumber(columnText) == 0)
                    continue;

                const std::string_view file = TrimAscii(text.substr(0, open));
                if (file.empty())
                    continue;
                const std::string_view rest = TrimAscii(text.substr(close + 1));
                if (!SplitSeverity(rest, outLevel, outCode, outMessage))
                    continue;

                outFile = file;
                outLine = line;
                outColumn = columnText.empty() ? 0 : ToLineNumber(columnText);
                outRest = rest;
                return true;
            }
            return false;
        }

        // `: error C2065: 'x': undeclared identifier` -> severity Error, code
        // C2065, message "'x': undeclared identifier". Returns false when the
        // text does not begin with a severity keyword.
        inline bool SplitSeverity(std::string_view rest, Severity& outLevel, std::string_view& outCode,
                                  std::string_view& outMessage)
        {
            rest = TrimAscii(rest);
            if (!rest.empty() && rest.front() == ':')
                rest = TrimAscii(rest.substr(1));

            constexpr std::string_view kFatal = "fatal error";
            if (rest.starts_with(kFatal))
            {
                outLevel = Severity::Error;
                rest = TrimAscii(rest.substr(kFatal.size()));
            }
            else if (rest.starts_with("error"))
            {
                outLevel = Severity::Error;
                rest = TrimAscii(rest.substr(5));
            }
            else if (rest.starts_with("warning"))
            {
                outLevel = Severity::Warning;
                rest = TrimAscii(rest.substr(7));
            }
            else if (rest.starts_with("note"))
            {
                outLevel = Severity::Note;
                rest = TrimAscii(rest.substr(4));
            }
            else
            {
                return false;
            }

            // An MSVC/clang-cl diagnostic id sits between the severity and the
            // colon: `error C2065:`. GCC/Clang emit `error:` with no id.
            outCode = {};
            if (const sizet colon = rest.find(':'); colon != std::string_view::npos && colon > 0)
            {
                const std::string_view candidate = TrimAscii(rest.substr(0, colon));
                const bool looksLikeCode =
                    !candidate.empty() && candidate.size() <= 12 &&
                    std::all_of(candidate.begin(), candidate.end(),
                                [](char c)
                                { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }) &&
                    std::any_of(candidate.begin(), candidate.end(), [](char c)
                                { return c >= 'A' && c <= 'Z'; }) &&
                    std::any_of(candidate.begin(), candidate.end(), [](char c)
                                { return c >= '0' && c <= '9'; });
                if (looksLikeCode)
                {
                    outCode = candidate;
                    rest = rest.substr(colon + 1);
                }
            }
            outMessage = TrimAscii(rest);
            if (!outMessage.empty() && outMessage.front() == ':')
                outMessage = TrimAscii(outMessage.substr(1));
            return !outMessage.empty();
        }
    } // namespace Detail

    // Parse ONE console line into a diagnostic record, or nothing.
    //
    // Four shapes are recognised, and they are the four this repo's toolchains
    // actually emit:
    //
    //   MSVC cl        C:\p\f.cpp(42): error C2065: 'x': undeclared identifier
    //   clang-cl       C:\p\f.cpp(42,9): warning: unused variable 'x' [-Wunused]
    //   clang/gcc      /p/f.cpp:42:9: error: use of undeclared identifier 'x'
    //   MSVC link      f.obj : error LNK2019: unresolved external symbol ...
    //                  LINK : fatal error LNK1104: cannot open file 'x.lib'
    //
    // Nothing here tries to be a compiler-output grammar. A line that does not
    // match is not a diagnostic as far as this command is concerned, and the
    // console tail is still returned on failure so nothing is actually lost.
    [[nodiscard]] inline std::optional<Diagnostic> ParseDiagnosticLine(std::string_view rawLine)
    {
        const std::string_view line = Detail::TrimAscii(Detail::StripProjectSuffix(Detail::TrimAscii(rawLine)));
        if (line.empty())
            return std::nullopt;

        Diagnostic diagnostic;
        std::string_view file;
        std::string_view rest;
        int lineNumber = 0;
        int column = 0;

        // 1/2. MSVC and clang-cl: `<file>(<line>[,<col>])<rest>`.
        //
        // The severity is validated INSIDE the split (it is how the right
        // parenthesis is chosen), so a failure here means "not this shape" and
        // must fall through to 3 and 4 rather than end the parse — a gcc line
        // whose message contains `foo(3)` used to be mis-split here and then
        // discarded before the gcc branch ever saw it.
        {
            Severity level = Severity::Error;
            std::string_view code;
            std::string_view message;
            if (Detail::SplitMsvcLocation(line, file, lineNumber, column, rest, level, code, message))
            {
                diagnostic.Level = level;
                diagnostic.File = std::string(file);
                diagnostic.Line = lineNumber;
                diagnostic.Column = column;
                diagnostic.Code = std::string(code);
                diagnostic.Message = std::string(message);
                return diagnostic;
            }
        }

        // 3. clang / gcc: `<file>:<line>:<col>: <severity>: <message>`. Scanned
        //    from the RIGHT so a Windows drive letter (`C:\...`) cannot be read
        //    as the first field separator.
        {
            const sizet third = line.find(": ");
            if (third != std::string_view::npos && third >= 3)
            {
                const std::string_view head = line.substr(0, third);
                const sizet lastColon = head.rfind(':');
                if (lastColon != std::string_view::npos && lastColon > 0)
                {
                    const sizet firstColon = head.rfind(':', lastColon - 1);
                    if (firstColon != std::string_view::npos && firstColon > 0)
                    {
                        const std::string_view lineText = head.substr(firstColon + 1, lastColon - firstColon - 1);
                        const std::string_view columnText = head.substr(lastColon + 1);
                        const int parsedLine = Detail::ToLineNumber(lineText);
                        const int parsedColumn = Detail::ToLineNumber(columnText);
                        if (parsedLine > 0 && parsedColumn > 0)
                        {
                            Severity level = Severity::Error;
                            std::string_view code;
                            std::string_view message;
                            if (Detail::SplitSeverity(line.substr(third), level, code, message))
                            {
                                diagnostic.Level = level;
                                diagnostic.File = std::string(head.substr(0, firstColon));
                                diagnostic.Line = parsedLine;
                                diagnostic.Column = parsedColumn;
                                diagnostic.Code = std::string(code);
                                diagnostic.Message = std::string(message);
                                return diagnostic;
                            }
                        }
                    }
                }
            }
        }

        // 4. The linker and the toolchain drivers: no location at all.
        //    `f.obj : error LNK2019: ...` / `LINK : fatal error LNK1104: ...`
        if (const sizet marker = line.find(" : "); marker != std::string_view::npos)
        {
            Severity level = Severity::Error;
            std::string_view code;
            std::string_view message;
            if (Detail::SplitSeverity(line.substr(marker + 3), level, code, message))
            {
                diagnostic.Level = level;
                // The left half is an object file or the literal "LINK", not a
                // source location — reported as the file with no line, because
                // "which object could not be linked" is the whole of what the
                // linker knows.
                const std::string_view origin = Detail::TrimAscii(line.substr(0, marker));
                if (origin != "LINK" && origin != "link")
                    diagnostic.File = std::string(origin);
                diagnostic.Code = std::string(code);
                diagnostic.Message = std::string(message);
                return diagnostic;
            }
        }

        return std::nullopt;
    }

    // Normalize a diagnostic's file to a repo-relative path with forward
    // slashes. `repoRoot` is compared case-insensitively because MSVC and ninja
    // disagree about the case of a Windows drive letter within one build.
    // Returns the input unchanged when it does not lie under the root — never
    // empty, because a path outside the tree (a vcpkg header) is still the
    // truthful answer.
    [[nodiscard]] inline std::string RelativizePath(std::string_view path, std::string_view repoRoot)
    {
        std::string normalized(path);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        if (repoRoot.empty())
            return normalized;

        std::string root(repoRoot);
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/')
            root.pop_back();
        if (root.empty() || normalized.size() <= root.size())
            return normalized;

        const auto lower = [](char c)
        { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
        for (sizet i = 0; i < root.size(); ++i)
        {
            if (lower(normalized[i]) != lower(root[i]))
                return normalized;
        }
        if (normalized[root.size()] != '/')
            return normalized;
        return normalized.substr(root.size() + 1);
    }

    // Collapse a lexical path: split on '/', drop '.', pop on '..'. No
    // filesystem access, so it works on a path whose file no longer exists and
    // stays testable headlessly. Leading '..' that cannot be popped is kept, so
    // a path that genuinely escapes the base is visibly unresolved rather than
    // silently rebased.
    [[nodiscard]] inline std::string CollapseLexicalPath(std::string_view path)
    {
        std::vector<std::string_view> parts;
        sizet cursor = 0;
        while (cursor <= path.size())
        {
            const sizet slash = std::min(path.find('/', cursor), path.size());
            const std::string_view part = path.substr(cursor, slash - cursor);
            cursor = slash + 1;
            if (part.empty() || part == ".")
                continue;
            if (part == ".." && !parts.empty() && parts.back() != "..")
                parts.pop_back();
            else
                parts.push_back(part);
            if (slash == path.size())
                break;
        }
        std::string out;
        for (const std::string_view part : parts)
        {
            if (!out.empty())
                out.push_back('/');
            out += part;
        }
        return out;
    }

    // Turn a diagnostic's raw file into a path a caller can open.
    //
    // The two toolchains disagree about what a path is relative TO. ninja runs
    // the compiler with the build directory as its working directory, so a
    // compiler reports `__FILE__` relative to THAT: the live run of this command
    // on 2026-09-10 produced `../OloEngine/src/OloEngine/Core/UUID.h` and
    // `vcpkg_installed/x64-windows-static-md/include/entt/...`. Relativizing
    // those against the repo root alone leaves them exactly as they arrived,
    // which is not a path anything can open from the root.
    //
    // So a RELATIVE path is joined to the build directory and collapsed; an
    // ABSOLUTE one is relativized against the repo root and otherwise left
    // alone, because an absolute path outside the tree (an MSVC or vcpkg header)
    // is the truthful answer.
    [[nodiscard]] inline std::string ResolveDiagnosticFile(std::string_view rawPath, std::string_view repoRoot,
                                                           std::string_view buildDirRelative)
    {
        std::string normalized(rawPath);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        if (normalized.empty())
            return normalized;

        // "C:/..." or "/...": absolute either way.
        const bool absolute = normalized.front() == '/' ||
                              (normalized.size() > 2 && normalized[1] == ':' && normalized[2] == '/');
        if (absolute)
            return RelativizePath(normalized, repoRoot);
        if (buildDirRelative.empty())
            return CollapseLexicalPath(normalized);
        return CollapseLexicalPath(std::string(buildDirRelative) + "/" + normalized);
    }

    // Collapse identical records, counting them. MSVC emits the same header
    // warning once per translation unit that includes it — 40 copies of one
    // `C4996` is one defect, and reporting it 40 times pushes the real error
    // past any cap the caller set.
    //
    // Order is preserved (first occurrence wins its position), because the first
    // error is the one that matters and a re-sorted list buries it.
    [[nodiscard]] inline std::vector<Diagnostic> Deduplicate(const std::vector<Diagnostic>& diagnostics)
    {
        std::vector<Diagnostic> out;
        out.reserve(diagnostics.size());
        for (const Diagnostic& diagnostic : diagnostics)
        {
            const auto found = std::find(out.begin(), out.end(), diagnostic);
            if (found != out.end())
                ++found->Occurrences;
            else
                out.push_back(diagnostic);
        }
        return out;
    }

    // Scan a whole console log. Each record's file is resolved by
    // ResolveDiagnosticFile — see there for why the build directory is needed
    // as well as the repo root.
    [[nodiscard]] inline std::vector<Diagnostic> ParseDiagnostics(std::string_view consoleText,
                                                                  std::string_view repoRoot,
                                                                  std::string_view buildDirRelative = {})
    {
        std::vector<Diagnostic> found;
        sizet cursor = 0;
        while (cursor <= consoleText.size())
        {
            const sizet lineEnd = std::min(consoleText.find('\n', cursor), consoleText.size());
            const std::string_view line = consoleText.substr(cursor, lineEnd - cursor);
            cursor = lineEnd + 1;
            if (std::optional<Diagnostic> diagnostic = ParseDiagnosticLine(line))
            {
                if (!diagnostic->File.empty())
                    diagnostic->File = ResolveDiagnosticFile(diagnostic->File, repoRoot, buildDirRelative);
                found.push_back(std::move(*diagnostic));
            }
            if (lineEnd == consoleText.size())
                break;
        }
        return Deduplicate(found);
    }

    struct DiagnosticCounts
    {
        sizet Errors = 0;
        sizet Warnings = 0;
        sizet Notes = 0;
    };

    // Counts UNIQUE records, not occurrences: "3 errors" should mean three
    // things to fix, and a header warning included by 40 TUs is one.
    [[nodiscard]] inline DiagnosticCounts CountDiagnostics(const std::vector<Diagnostic>& diagnostics)
    {
        DiagnosticCounts counts;
        for (const Diagnostic& diagnostic : diagnostics)
        {
            switch (diagnostic.Level)
            {
                case Severity::Error:
                    ++counts.Errors;
                    break;
                case Severity::Warning:
                    ++counts.Warnings;
                    break;
                case Severity::Note:
                    ++counts.Notes;
                    break;
            }
        }
        return counts;
    }

    [[nodiscard]] inline Json DiagnosticJson(const Diagnostic& diagnostic)
    {
        Json entry;
        entry["severity"] = SeverityName(diagnostic.Level);
        if (!diagnostic.File.empty())
            entry["file"] = diagnostic.File;
        if (diagnostic.Line > 0)
            entry["line"] = diagnostic.Line;
        if (diagnostic.Column > 0)
            entry["column"] = diagnostic.Column;
        if (!diagnostic.Code.empty())
            entry["code"] = diagnostic.Code;
        entry["message"] = diagnostic.Message;
        if (diagnostic.Occurrences > 1)
            entry["occurrences"] = diagnostic.Occurrences;
        return entry;
    }

    // ---- what build-lock.ps1 said -----------------------------------------
    //
    // The lock writes its own story to the same stdout the build inherits, so
    // the console log carries a transcript of the concurrency decision. Reading
    // it is what turns "the lock is somewhere upstream of this" into a
    // structured field the caller can act on — and it is the ONLY way to see
    // the stand-down, which otherwise looks exactly like a successful build.

    enum class LockOutcome : u8
    {
        Acquired = 0, // the build ran under the lock
        Superseded,   // stood down with EXIT 0 and built NOTHING (Test-Superseded)
        Orphaned,     // the launching process died; the lock killed the build tree
        NeverStarted, // no `[build-lock] acquired` line at all: a wait that timed out, or a crash
    };

    [[nodiscard]] inline const char* LockOutcomeName(LockOutcome outcome)
    {
        switch (outcome)
        {
            case LockOutcome::Acquired:
                return "acquired";
            case LockOutcome::Superseded:
                return "superseded";
            case LockOutcome::Orphaned:
                return "orphaned";
            case LockOutcome::NeverStarted:
                return "never-started";
        }
        return "unknown";
    }

    struct LockTranscript
    {
        LockOutcome Outcome = LockOutcome::NeverStarted;
        bool Waited = false;            // at least one `waiting —` line: the lock was contended
        int Jobs = 0;                   // the parallelism the lock actually chose, 0 if unreported
        int HeldByPid = 0;              // the holder we were queued behind, when it said
        std::string HeldByWorktree;     // ditto
        std::string ConcurrencyRefusal; // the `not building alongside` reason, when it gave one
        std::vector<std::string> Lines; // every `[build-lock]` line, verbatim and in order
    };

    // Read every `[build-lock] ...` line out of a console log.
    [[nodiscard]] inline LockTranscript ReadLockTranscript(std::string_view consoleText)
    {
        constexpr std::string_view kPrefix = "[build-lock]";
        LockTranscript transcript;

        sizet cursor = 0;
        while (cursor <= consoleText.size())
        {
            const sizet lineEnd = std::min(consoleText.find('\n', cursor), consoleText.size());
            const std::string_view raw = Detail::TrimAscii(consoleText.substr(cursor, lineEnd - cursor));
            cursor = lineEnd + 1;

            // The marker is FOUND, not required at column 0. The script's wait
            // timeout is a PowerShell `throw`, and the host decorates a
            // terminating error before printing it ("Exception: [build-lock]
            // timed out after 5m waiting for pid=11 (...)"). That line is the
            // one that names the holder, so anchoring at the start would drop
            // precisely the refusal a caller needs to read.
            const sizet marker = raw.find(kPrefix);
            // A line still carrying an UNINTERPOLATED PowerShell variable is the
            // host echoing the script's SOURCE, not something the script said.
            // A terminating error prints the offending source line above the
            // message, so a `throw "[build-lock] timed out after ${TimeoutMinutes}m …"`
            // contributes a truncated look-alike right next to the real one.
            // Observed 2026-09-10 driving the live editor. Real output can never
            // contain "${" — PowerShell would have expanded it.
            const bool isEchoedSource = raw.find("${") != std::string_view::npos;
            if (marker == std::string_view::npos || isEchoedSource)
            {
                if (lineEnd == consoleText.size())
                    break;
                continue;
            }
            transcript.Lines.emplace_back(raw.substr(marker));
            const std::string_view body = Detail::TrimAscii(raw.substr(marker + kPrefix.size()));

            if (body.starts_with("acquired"))
            {
                // Guarded, not assigned: the stand-down path exits before ever
                // acquiring, so the two cannot both appear in one run — but if
                // they somehow did (two runs' output interleaved into one log),
                // the safe reading is "nothing was built", and an unguarded
                // assignment here would quietly pick the unsafe one. The comment
                // on the branch below used to claim this without the code doing it.
                if (transcript.Outcome != LockOutcome::Superseded && transcript.Outcome != LockOutcome::Orphaned)
                    transcript.Outcome = LockOutcome::Acquired;
            }
            else if (body.starts_with("superseded"))
            {
                transcript.Outcome = LockOutcome::Superseded;
            }
            else if (body.starts_with("the process that launched this build"))
            {
                transcript.Outcome = LockOutcome::Orphaned;
            }
            // "timed out after 5m waiting for pid=11 (C:/repos/y)" carries the
            // holder in the same `pid=` shape, and it is the ONLY line a refused
            // acquire produces — the outcome stays NeverStarted, which is what
            // the absence of an `acquired` line already says.
            else if (body.starts_with("waiting") || body.starts_with("timed out"))
            {
                transcript.Waited = true;
                // `waiting — next in line; held by pid=1234 in C:/repos/x`
                if (const sizet at = body.find("pid="); at != std::string_view::npos)
                {
                    const std::string_view after = body.substr(at + 4);
                    sizet digits = 0;
                    while (digits < after.size() && after[digits] >= '0' && after[digits] <= '9')
                        ++digits;
                    if (transcript.HeldByPid == 0)
                        transcript.HeldByPid = Detail::ToLineNumber(after.substr(0, digits));
                    if (const sizet inAt = after.find(" in "); inAt != std::string_view::npos &&
                                                               transcript.HeldByWorktree.empty())
                        transcript.HeldByWorktree = std::string(Detail::TrimAscii(after.substr(inAt + 4)));
                }
            }
            else if (constexpr std::string_view kNotAlongside = "not building alongside the current build";
                     body.starts_with(kNotAlongside))
            {
                // The reason follows a dash — which is an EM dash in the script's
                // source and a plain ASCII '-' by the time it reaches a redirected
                // log, because the PowerShell host transcodes its output to the
                // console code page on the way out (observed 2026-09-10 on this
                // box: `not building alongside the current build - only 22.6 GB
                // free`). So the separator is skipped by character class rather
                // than matched as a literal; anything else here would silently
                // return the whole line as the "reason" on one of the two.
                transcript.ConcurrencyRefusal =
                    std::string(Detail::TrimDashes(body.substr(kNotAlongside.size())));
            }
            else if (body.starts_with("parallelism: -j"))
            {
                const std::string_view after = body.substr(std::string_view("parallelism: -j").size());
                sizet digits = 0;
                while (digits < after.size() && after[digits] >= '0' && after[digits] <= '9')
                    ++digits;
                transcript.Jobs = Detail::ToLineNumber(after.substr(0, digits));
            }

            if (lineEnd == consoleText.size())
                break;
        }
        return transcript;
    }

    // Ninja prints `[123/4567] Building CXX object ...` for every edge. Returns
    // the most recent one, which is the only honest progress signal a build
    // gives — MSBuild prints none, so a `build/` tree simply reports no fraction
    // rather than a made-up one.
    struct BuildProgress
    {
        sizet Done = 0;
        sizet Total = 0;

        [[nodiscard]] bool Known() const
        {
            return Total > 0 && Done <= Total;
        }
    };

    [[nodiscard]] inline BuildProgress ReadLastNinjaProgress(std::string_view consoleText)
    {
        BuildProgress progress;
        sizet at = consoleText.rfind('[');
        while (at != std::string_view::npos)
        {
            const sizet close = consoleText.find(']', at);
            if (close != std::string_view::npos && close - at <= 20)
            {
                const std::string_view inside = consoleText.substr(at + 1, close - at - 1);
                if (const sizet slash = inside.find('/');
                    slash != std::string_view::npos && Detail::IsDigits(inside.substr(0, slash)) &&
                    Detail::IsDigits(inside.substr(slash + 1)))
                {
                    progress.Done = static_cast<sizet>(Detail::ToLineNumber(inside.substr(0, slash)));
                    progress.Total = static_cast<sizet>(Detail::ToLineNumber(inside.substr(slash + 1)));
                    if (progress.Known())
                        return progress;
                    progress = {};
                }
            }
            if (at == 0)
                break;
            at = consoleText.rfind('[', at - 1);
        }
        return progress;
    }

    // ---- the freshness verdict --------------------------------------------
    //
    // The reason this file exists. See the header comment.

    // A target's primary output, stat'd on both sides of the build.
    struct ArtifactState
    {
        std::string Path; // repo-relative
        bool Exists = false;
        u64 SizeBytes = 0;
        std::string ModifiedUtc; // ISO-8601; empty when it could not be read
        // Seconds between the build STARTING and the artefact's mtime. Negative
        // means the artefact predates the build, i.e. nothing replaced it.
        f64 AgeRelativeToStartSeconds = 0.0;
    };

    enum class TargetOutcome : u8
    {
        Rebuilt = 0,     // the build ran and the artefact was replaced
        UpToDate,        // the build ran, had nothing to do, and the artefact is the previous one
        Failed,          // the build reported failure
        MissingArtifact, // the build reported SUCCESS and there is no artefact — never a pass
        NotBuilt,        // the lock stood down / never acquired: nothing ran at all
        Skipped,         // an earlier target in the same call failed, so this one was never attempted
    };

    [[nodiscard]] inline const char* TargetOutcomeName(TargetOutcome outcome)
    {
        switch (outcome)
        {
            case TargetOutcome::Rebuilt:
                return "rebuilt";
            case TargetOutcome::UpToDate:
                return "up-to-date";
            case TargetOutcome::Failed:
                return "failed";
            case TargetOutcome::MissingArtifact:
                return "missing-artifact";
            case TargetOutcome::NotBuilt:
                return "not-built";
            case TargetOutcome::Skipped:
                return "skipped";
        }
        return "unknown";
    }

    // Only Rebuilt and UpToDate are successes, and they are distinguished
    // rather than merged: "up to date" is the answer a caller must be able to
    // see, because it is what a stale artefact looks like from the outside.
    [[nodiscard]] inline bool IsSuccess(TargetOutcome outcome)
    {
        return outcome == TargetOutcome::Rebuilt || outcome == TargetOutcome::UpToDate;
    }

    // Combine the three independent inputs into one verdict.
    //
    // `lockOutcome` FIRST, because it can veto a zero exit code: a superseded
    // stand-down exits 0 having built nothing, and an orphan kill exits with
    // whatever the kill happened to report. Only then does the exit code
    // matter, and only then the artefact.
    [[nodiscard]] inline TargetOutcome Verdict(LockOutcome lockOutcome, int exitCode, const ArtifactState& after)
    {
        if (lockOutcome == LockOutcome::Superseded || lockOutcome == LockOutcome::NeverStarted)
            return TargetOutcome::NotBuilt;
        if (lockOutcome == LockOutcome::Orphaned || exitCode != 0)
            return TargetOutcome::Failed;
        if (!after.Exists)
            return TargetOutcome::MissingArtifact;
        // The mtime is compared against the moment the build STARTED, not
        // against the previous stat. Comparing to the previous stat would call a
        // rebuild "up to date" whenever the compiler reproduced a byte-identical
        // file at the same timestamp granularity, and it needs the before-state
        // to be readable, which it is not for a first build.
        return after.AgeRelativeToStartSeconds >= 0.0 ? TargetOutcome::Rebuilt : TargetOutcome::UpToDate;
    }

    struct TargetResult
    {
        std::string Target;
        // The cmake line the lock was asked to run, verbatim. Reported so a
        // caller staring at a failure can reproduce it in a shell without
        // reconstructing it from three separate fields.
        std::string Command;
        TargetOutcome Outcome = TargetOutcome::NotBuilt;
        int ExitCode = -1;
        f64 WallSeconds = 0.0;
        // WallSeconds split at the moment the lock was acquired. Measured live
        // on 2026-09-10: building a 15 KB DLL reported 1474 wall seconds, of
        // which 1451 were queue. Reporting only the total invites a caller to
        // conclude the build is slow when the machine was simply busy — and the
        // two numbers lead to opposite actions. Negative => the acquire was
        // never observed (the child finished between polls, or never acquired),
        // and the fields are omitted rather than guessed.
        f64 QueuedSeconds = -1.0;
        f64 BuildSeconds = -1.0;
        ArtifactState Before;
        ArtifactState After;
        LockTranscript Lock;
        std::vector<Diagnostic> Diagnostics;
        std::string Note; // why, for every outcome that needs a sentence
    };

    [[nodiscard]] inline Json ArtifactJson(const ArtifactState& state)
    {
        Json entry;
        entry["path"] = state.Path;
        entry["exists"] = state.Exists;
        if (state.Exists)
        {
            entry["sizeBytes"] = state.SizeBytes;
            if (!state.ModifiedUtc.empty())
                entry["modifiedUtc"] = state.ModifiedUtc;
        }
        return entry;
    }

    [[nodiscard]] inline Json LockJson(const LockTranscript& lock)
    {
        Json entry;
        entry["outcome"] = LockOutcomeName(lock.Outcome);
        entry["contended"] = lock.Waited;
        if (lock.Jobs > 0)
            entry["jobs"] = lock.Jobs;
        if (lock.HeldByPid > 0)
        {
            entry["heldByPid"] = lock.HeldByPid;
            if (!lock.HeldByWorktree.empty())
                entry["heldByWorktree"] = lock.HeldByWorktree;
        }
        if (!lock.ConcurrencyRefusal.empty())
            entry["concurrencyRefusal"] = lock.ConcurrencyRefusal;
        if (!lock.Lines.empty())
            entry["transcript"] = lock.Lines;
        return entry;
    }

    [[nodiscard]] inline Json TargetResultJson(const TargetResult& result, sizet maxDiagnostics)
    {
        Json entry;
        entry["target"] = result.Target;
        if (!result.Command.empty())
            entry["command"] = result.Command;
        entry["outcome"] = TargetOutcomeName(result.Outcome);
        entry["success"] = IsSuccess(result.Outcome);
        entry["exitCode"] = result.ExitCode;
        entry["wallSeconds"] = result.WallSeconds;
        if (result.QueuedSeconds >= 0.0)
            entry["queuedSeconds"] = result.QueuedSeconds;
        if (result.BuildSeconds >= 0.0)
            entry["buildSeconds"] = result.BuildSeconds;
        entry["artifact"] = ArtifactJson(result.After);
        // The BEFORE state is reported too, and only when it differs, because
        // "the artefact is 40 minutes old and this build did not touch it" is
        // exactly the question an exit code cannot answer.
        if (result.Before.Exists && result.Before.ModifiedUtc != result.After.ModifiedUtc)
            entry["artifactBefore"] = ArtifactJson(result.Before);
        entry["lock"] = LockJson(result.Lock);

        const DiagnosticCounts counts = CountDiagnostics(result.Diagnostics);
        entry["errorCount"] = counts.Errors;
        entry["warningCount"] = counts.Warnings;

        Json diagnostics = Json::array();
        sizet omitted = 0;
        // Errors before warnings: a cap that dropped the one error in favour of
        // 200 warnings would hide the reason the build failed.
        for (const Severity level : { Severity::Error, Severity::Warning, Severity::Note })
        {
            for (const Diagnostic& diagnostic : result.Diagnostics)
            {
                if (diagnostic.Level != level)
                    continue;
                if (diagnostics.size() >= maxDiagnostics)
                {
                    ++omitted;
                    continue;
                }
                diagnostics.push_back(DiagnosticJson(diagnostic));
            }
        }
        entry["diagnostics"] = std::move(diagnostics);
        entry["diagnosticsOmitted"] = omitted;
        if (!result.Note.empty())
            entry["note"] = result.Note;
        return entry;
    }

    // The last `maxChars` of a console log, marked when anything was dropped.
    // Keeps the END, where the failure is.
    [[nodiscard]] inline std::string TailOf(std::string_view text, sizet maxChars)
    {
        if (maxChars == 0 || text.size() <= maxChars)
            return std::string(text);
        return "...[" + std::to_string(text.size() - maxChars) + " earlier characters omitted]...\n" +
               std::string(text.substr(text.size() - maxChars));
    }
} // namespace OloEngine::Automation::BuildInvocation
