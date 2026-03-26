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
    };

} // namespace OloEngine
