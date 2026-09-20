#pragma once

// The sandboxed capture directory behind `olo_screenshot`'s optional `path`
// (issue #607).
//
// WHY A PATH ARGUMENT AT ALL. The PNG comes back inline as base64, or as an
// ephemeral `olo://capture` resource. A session driving the server over RAW
// HTTP — the only option when the `olo_*` tools are not registered as MCP
// tools, which is every non-interactive session, because it never gets the
// reconnect that would surface them — must therefore base64-decode every
// capture itself before it can look at one. A path turns that into a file it
// can open.
//
// WHY THE REJECTIONS ARE THE DESIGN, NOT AN AFTERTHOUGHT. An unconstrained
// path would hand the diagnostics server an ARBITRARY-WRITE primitive, over
// a tool that is otherwise read-only and unauthenticated on localhost. So the
// argument is not a path: it is a NAME resolved under one fixed directory.
// Absolute paths and `..` are refused, never sanitised — a silently rewritten
// path is worse than a rejected one, because the caller then believes it wrote
// somewhere it did not.
//
// The lexical half (`ValidateRelative`) touches no filesystem and is what the
// unit tests pin; `Resolve` adds the symlink-escape check and the directory
// creation, which need a real tree. Same split, and the same containment
// argument, as ResolveGoldenPath in McpToolsRender.cpp — that one predates
// this header and stays where it is because its root and its extension rule
// are different.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace OloEngine::MCP::CapturePath
{
    // The one directory a capture may land in, relative to the editor's working
    // directory (OloEditor/ — see CLAUDE.md § Build & run). Git-ignored: these
    // are session artefacts, not project content.
    [[nodiscard]] inline std::filesystem::path Root()
    {
        return std::filesystem::path("assets") / "mcp-captures";
    }

    [[nodiscard]] inline std::string RootDisplay()
    {
        return "assets/mcp-captures/";
    }

    // The lexical, filesystem-free half. On success `outRelative` is the path
    // relative to the editor CWD (so a bare "shot.png" becomes
    // assets/mcp-captures/shot.png) with a .png extension forced on. Returns a
    // human-readable error otherwise.
    //
    // Note the order: every rejection is decided on the path AS WRITTEN, before
    // the root is prepended. Prepending first and checking afterwards is the
    // classic way to get this wrong — "../../x" under a root still normalises
    // to something inside it whenever the root is deep enough, so the check
    // would pass for some roots and fail for others.
    [[nodiscard]] inline std::optional<std::string> ValidateRelative(std::string_view input,
                                                                     std::filesystem::path& outRelative)
    {
        namespace fs = std::filesystem;
        if (input.empty())
            return "Invalid 'path': must not be empty. It is a name resolved under " + RootDisplay() + ".";

        // Backslashes are folded to '/' FIRST, and on every platform. A
        // Windows-shaped name is what a Windows client sends, and on Linux
        // std::filesystem does not treat '\' as a separator at all — so
        // "a\..\..\escape.png" would be ONE filename there and sail past the
        // component loop below. Folding is a separator rewrite, not a semantic
        // one: it happens before every check, so it can only ever expose a
        // traversal, never hide one.
        std::string folded(input);
        std::replace(folded.begin(), folded.end(), '\\', '/');

        // A colon, on EVERY platform. "C:evil.png" is drive-RELATIVE on
        // Windows: it has a root name but no root directory, so is_absolute()
        // is false for it and the check below would wave it through. On Linux
        // std::filesystem has no notion of a drive at all, so the same string
        // is one ordinary filename and the guard would differ per platform —
        // which is how a test passes on the dev box and the hole ships. A
        // colon is illegal in a Windows filename anyway, so nothing legitimate
        // is lost. (It also closes NTFS alternate data streams, "shot.png:x".)
        if (folded.find(':') != std::string::npos)
        {
            return "Invalid 'path': a drive letter or stream separator (':') is not allowed; 'path' must be "
                   "relative to " +
                   RootDisplay() + ".";
        }

        const fs::path raw{ folded };
        // Checked BEFORE lexically_normal: normalisation can collapse a leading
        // "x/.." pair and hide a traversal that was written down.
        if (raw.is_absolute() || raw.has_root_name() || raw.has_root_directory())
        {
            return "Invalid 'path': must be relative (no drive letter and no leading slash). It is resolved "
                   "under " +
                   RootDisplay() + ", and an absolute path is refused rather than rewritten.";
        }
        for (const auto& part : raw)
        {
            if (part == "..")
                return "Invalid 'path': parent-directory traversal ('..') is not allowed.";
        }

        const fs::path normalized = raw.lexically_normal();
        // Normalisation of a legal relative path can still produce a leading
        // ".." only if one was present, which the loop above already refused —
        // but a path of just "." normalises to empty, which would resolve to
        // the root directory itself rather than a file in it.
        if (normalized.empty() || normalized == ".")
            return "Invalid 'path': names no file. Pass a filename such as \"shot.png\".";
        for (const auto& part : normalized)
        {
            if (part == "..")
                return "Invalid 'path': parent-directory traversal ('..') is not allowed.";
        }
        if (!normalized.has_filename())
            return "Invalid 'path': must name a file, not a directory (it must not end in a separator).";

        // Accept either a bare name/subpath, or one already rooted at the
        // captures directory — so echoing back a path this tool returned works.
        const fs::path root = Root();
        const auto mm = std::mismatch(root.begin(), root.end(), normalized.begin(), normalized.end());
        fs::path candidate = (mm.first == root.end() ? normalized : (root / normalized)).lexically_normal();

        // Force .png: the bytes are always a PNG, and a caller that named the
        // file ".jpg" would otherwise get a file that lies about its format.
        if (const fs::path ext = candidate.extension(); ext != ".png" && ext != ".PNG")
            candidate += ".png";

        // CONTAINMENT, RE-CHECKED ON THE FINAL PATH — not on the input. Every
        // check above reasons about what the caller wrote; this one reasons
        // about what we are actually going to open, which is the only thing
        // that matters. It is not redundant: the input "assets/mcp-captures"
        // takes the already-under-root branch (it IS the root), the extension
        // is then appended, and the result is the SIBLING file
        // "assets/mcp-captures.png" — outside the directory, reached without a
        // '..' or an absolute path anywhere. The same append makes any input
        // that normalises to the root itself escape the same way.
        const auto contained = std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end());
        if (contained.first != root.end() || contained.second == candidate.end())
        {
            return "Invalid 'path': must name a file INSIDE " + RootDisplay() +
                   ", not the directory itself or a sibling of it.";
        }

        outRelative = std::move(candidate);
        return std::nullopt;
    }

    // The full resolve: `ValidateRelative`, then create the parent directory and
    // confirm the result really is inside the captures root once symlinks have
    // been followed. `outAbsolute` is what the tool reports back.
    //
    // The lexical checks stop '..' and absolute paths; they do NOT stop a
    // symlinked component inside the root redirecting the write outside it.
    // weakly_canonical resolves the existing prefix — which catches a symlinked
    // root — and handles the not-yet-created file lexically.
    [[nodiscard]] inline std::optional<std::string> Resolve(std::string_view input,
                                                            std::filesystem::path& outRelative,
                                                            std::filesystem::path& outAbsolute)
    {
        namespace fs = std::filesystem;
        if (auto error = ValidateRelative(input, outRelative))
            return error;

        std::error_code ec;
        fs::create_directories(outRelative.parent_path(), ec);
        if (ec)
        {
            return "Could not create the capture directory '" + outRelative.parent_path().generic_string() +
                   "': " + ec.message();
        }

        const fs::path canonicalRoot = fs::weakly_canonical(Root(), ec);
        if (ec)
            return "Could not resolve the capture root (" + RootDisplay() + ").";
        const fs::path canonicalOut = fs::weakly_canonical(outRelative, ec);
        if (ec)
            return "Could not resolve 'path' to a canonical location.";
        if (const fs::path rel = canonicalOut.lexically_relative(canonicalRoot); rel.empty() || *rel.begin() == "..")
        {
            return "Invalid 'path': resolves outside " + RootDisplay() +
                   " (possible symlink escape). The capture was not written.";
        }

        outAbsolute = canonicalOut;
        return std::nullopt;
    }
} // namespace OloEngine::MCP::CapturePath
