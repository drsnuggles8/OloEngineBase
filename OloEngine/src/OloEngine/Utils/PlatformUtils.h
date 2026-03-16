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

    enum class MessagePromptResult
    {
        Yes,
        No,
        Cancel
    };

#ifdef _WIN32
    class MessagePrompt
    {
      public:
        // Show a Yes / No / Cancel dialog. Returns which button was pressed.
        static MessagePromptResult YesNoCancel(const char* title, const char* message);
    };
#else
    // Non-Windows stub — returns No (discard) so unsaved-changes dialogs don't block the editor
    class MessagePrompt
    {
      public:
        static MessagePromptResult YesNoCancel([[maybe_unused]] const char* title, [[maybe_unused]] const char* message)
        {
            return MessagePromptResult::No;
        }
    };
#endif

    class Time
    {
      public:
        static f32 GetTime();
    };

} // namespace OloEngine
