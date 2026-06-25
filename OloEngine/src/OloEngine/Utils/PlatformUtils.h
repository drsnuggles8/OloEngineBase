#pragma once

#include <filesystem>
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

        // Reveal a path in the OS file manager (Explorer on Windows, the default
        // file manager via xdg-open on Linux). When `path` is a regular file it is
        // selected inside its parent folder on platforms that support selection
        // (Windows); otherwise the containing / target directory is opened. No-op
        // with a warning when the path does not exist or no file manager is
        // available. Non-blocking — returns as soon as the launch is dispatched.
        static void ShowInFileManager(const std::filesystem::path& path);
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
        // Seconds since startup (wall clock) — unless a mock time is set, in
        // which case GetTime() returns that fixed value. The mock exists so
        // deterministic renders/captures (e.g. golden-image tests of animated
        // water) can freeze the wave/scroll phase instead of sampling the clock.
        static f32 GetTime();

        // Freeze GetTime() to a fixed value (deterministic tests/captures).
        static void SetMockTime(f32 seconds)
        {
            s_MockTime = seconds;
            s_HasMockTime = true;
        }
        // Resume the real wall clock.
        static void ClearMockTime()
        {
            s_HasMockTime = false;
        }
        [[nodiscard]] static bool HasMockTime()
        {
            return s_HasMockTime;
        }
        [[nodiscard]] static f32 GetMockTime()
        {
            return s_MockTime;
        }

      private:
        static inline f32 s_MockTime = 0.0f;
        static inline bool s_HasMockTime = false;
    };

} // namespace OloEngine
