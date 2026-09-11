#pragma once

// Guarded, atomic file replacement for automation commands.
//
// Lifted out of AutomationSceneDocument.cpp when the asset commands (#1128)
// needed the identical discipline. Two implementations of "write a project file
// safely" would be two chances to get the guard subtly different, and the whole
// point of the guard is that it is never subtly different.
//
// Two properties, both load-bearing:
//
//   * ATOMIC. The bytes go to a sibling temporary and are moved into place with
//     one replace, so a reader ever sees the complete old file or the complete
//     new one, and a failed write leaves the old file untouched. The editor's
//     hot-reload watcher is one such reader, and it is known to RACE A LARGE
//     WRITE and cache a half-written asset -- writing in place is what causes
//     that. The temporary is a sibling rather than a %TEMP% file so the move is
//     a same-volume rename; its ".tmp" suffix is deliberately absent from the
//     asset extension map, so DecideFileWatchAction ignores it and dropping it
//     next to an asset cannot trigger a spurious auto-import.
//
//   * GUARDED. Every write states the bytes it expects to find first and
//     refuses when the file does not match. An automation command that read a
//     file, decided what to change and came back to write it must not clobber an
//     edit somebody made in between -- and undo must not restore over a file that
//     moved on. Refusing is the only correct answer; there is no safe fallback.
//
// THE GUARD IS A NARROW WINDOW, NOT AN ATOMIC COMPARE-AND-SWAP. The check reads
// the file and the replace is a separate syscall, so a writer that lands between
// the two is not detected and its bytes are lost. The window is re-checked
// immediately before the swap, which shrinks it to about the cost of one rename,
// but it does not close it: doing that needs a platform-specific conditional
// replacement (Win32 ReplaceFile with a backup, or an O_EXCL dance on POSIX) and
// the cooperation of every other writer, including the editor's own save path and
// whatever text editor the user has the file open in. Stated rather than implied,
// because a guard that reads as atomic and is not is worse than one whose limits
// are written down. In practice the contending writer is a human with the file
// open, and the seconds-to-minutes gap between their save and ours is many orders
// of magnitude wider than this window.

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <optional>
#include <string>

namespace OloEngine::Automation
{
    // A file's bytes, or nullopt for "the file does not exist". The absent state
    // is a first-class value, not an error: creating a file is a replace whose
    // expected contents are nullopt, and deleting one is a replace whose
    // replacement is nullopt.
    using FileContents = std::optional<std::string>;

    // Read path's bytes, or nullopt when it does not exist. Throws when the path
    // exists but cannot be read, or is not a regular file.
    [[nodiscard]] FileContents ReadFileContents(const std::filesystem::path& path);

    // Throw unless path currently holds exactly `expected`.
    void RequireFileContents(const std::filesystem::path& path, const FileContents& expected);

    // Replace path's contents, refusing unless it currently holds `expected`.
    // A null `replacement` removes the file. See the header comment for the
    // atomicity contract.
    void ReplaceFileContents(const std::filesystem::path& path, const FileContents& expected,
                             const FileContents& replacement);
} // namespace OloEngine::Automation
