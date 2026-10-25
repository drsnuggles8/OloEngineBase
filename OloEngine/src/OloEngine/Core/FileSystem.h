#pragma once

#include "OloEngine/Core/Buffer.h"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace OloEngine
{
    // How FileSystem::WriteFileAtomically writes. The defaults write binary
    // bytes into an existing directory with no extra disk sync.
    struct AtomicWriteOptions
    {
        // Open the temporary in text mode, so "\n" becomes "\r\n" on Windows.
        // The YAML writers have always written this way; keeping it keeps
        // their files byte-identical.
        bool TextMode = false;
        // Create the target's missing parent directories first.
        bool CreateParentDirectories = false;
        // Push the temporary's bytes to the disk before the rename, and the
        // rename after it (MOVEFILE_WRITE_THROUGH on Windows, an fsync of the
        // directory on POSIX), so a power cut after a reported success cannot
        // leave an empty or old file. Costs a disk round-trip or two per write.
        // The one false return that leaves the NEW file in place is a failed
        // directory sync: the bytes landed, their durability is not promised.
        bool Durable = false;
        // Called with the finished temporary just before it replaces the
        // target. Returning false abandons the write: the temporary is
        // removed, the target is untouched, and the write returns false
        // without logging, because refusing was the caller's decision.
        std::function<bool(const std::filesystem::path& temporary)> BeforeReplace;
    };

    class FileSystem
    {
      public:
        static Buffer ReadFileBinary(const std::filesystem::path& filepath);
        static std::string ReadFileText(const std::filesystem::path& filepath);

        // Replace `path` with `bytes` so that a reader sees either the old file
        // or the whole new one, never a partial write. The bytes go to a
        // uniquely named "<name>.<id>.tmp" beside the target (".tmp" is not an
        // asset extension, so the file watcher ignores it), which is checked
        // through open, write, flush and close and then renamed over the
        // target. A symlinked target is written through to the file it points
        // at. On failure the target is untouched, the temporary is removed,
        // the reason (naming the path and the OS error) is logged and copied
        // into `outError`, and the result is false.
        [[nodiscard]] static bool WriteFileAtomically(const std::filesystem::path& path, std::string_view bytes,
                                                      const AtomicWriteOptions& options = {},
                                                      std::string* outError = nullptr);
        [[nodiscard]] static bool WriteFileAtomically(const std::filesystem::path& path, std::span<const u8> bytes,
                                                      const AtomicWriteOptions& options = {},
                                                      std::string* outError = nullptr);
        // For a writer that streams its output rather than building it in
        // memory first. `write` returns false to abandon the write, which,
        // like a BeforeReplace veto, is not logged; a stream it leaves failed
        // is a failed write and is.
        [[nodiscard]] static bool WriteFileAtomically(const std::filesystem::path& path,
                                                      const std::function<bool(std::ostream&)>& write,
                                                      const AtomicWriteOptions& options = {},
                                                      std::string* outError = nullptr);

        // Returns true if pathA exists, pathB exists, and pathA was last-modified more recently than pathB.
        static bool IsNewer(const std::filesystem::path& pathA, const std::filesystem::path& pathB);

        // Absolute directory containing the running executable, or an empty path
        // when the platform cannot answer. Anchors config lookups so a process
        // launched with an unexpected working directory (a shortcut with a stale
        // "Start in", a packaged game started from a parent directory) still
        // finds files shipped next to its exe (#691).
        static std::filesystem::path GetExecutableDirectory();
    };

} // namespace OloEngine
