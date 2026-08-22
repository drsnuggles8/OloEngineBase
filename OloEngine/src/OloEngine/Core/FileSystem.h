#pragma once

#include "OloEngine/Core/Buffer.h"

#include <filesystem>

namespace OloEngine
{
    class FileSystem
    {
      public:
        static Buffer ReadFileBinary(const std::filesystem::path& filepath);
        static std::string ReadFileText(const std::filesystem::path& filepath);

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
