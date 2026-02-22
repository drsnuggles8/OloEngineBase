#pragma once

#include <string>
#include <optional>

namespace OloEngine
{
    class FileDialogs
    {
      public:
        // These return empty strings if cancelled
        // initialDir overrides the OS-remembered last-used directory when provided
        static std::string OpenFile(const char* filter, const char* initialDir = nullptr);
        static std::string SaveFile(const char* filter, const char* initialDir = nullptr);
    };

    class Time
    {
      public:
        static f32 GetTime();
    };

} // namespace OloEngine
