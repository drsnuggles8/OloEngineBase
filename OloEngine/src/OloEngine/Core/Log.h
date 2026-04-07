#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
#include <memory>
#include <unordered_map>
#include <string>
#include <string_view>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "OloEngine/Core/Base.h"

// This ignores all warnings raised inside the following external headers
#pragma warning(push, 0)
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#pragma warning(pop)

// Forward-declare to avoid exposing spdlog sink internals in the public header
namespace spdlog::sinks
{
    template<typename Mutex>
    class ringbuffer_sink;
}

#if !defined(OLO_DIST) && defined(OLO_PLATFORM_WINDOWS)
#define OLO_ASSERT_MESSAGE_BOX 1
#else
#define OLO_ASSERT_MESSAGE_BOX 0
#endif

namespace OloEngine
{
#if OLO_ASSERT_MESSAGE_BOX
    // Defined in Log.cpp — keeps <Windows.h> out of this header
    void ShowAssertMessageBox(const char* message);
#endif

    class Log
    {
      public:
        enum class Type : u8
        {
            Core = 0,
            Client = 1
        };

        enum class Level : u8
        {
            Trace = 0,
            Info,
            Warn,
            Error,
            Fatal
        };

        struct TagDetails
        {
            bool Enabled = true;
            Level LevelFilter = Level::Trace;
        };

      public:
        // Singleton — created on first call via heap allocation, intentionally
        // leaked so it survives static teardown (avoids use-after-free when
        // other statics log during destruction).
        static Log& Get();

        // Explicit initialization entry point — call early in main() to trigger
        // singleton construction without requiring the caller to store the result.
        static void Initialize()
        {
            (void)Get();
        }

        // Returns a pointer to the singleton if it has already been constructed,
        // or nullptr if Get() has never been called.  Safe to call from crash
        // handlers and other contexts where heavy initialization is undesirable.
        static Log* GetIfInitialized()
        {
            return s_Initialized.load(std::memory_order_relaxed) ? &Get() : nullptr;
        }

        // Non-copyable, non-moveable
        Log(Log const&) = delete;
        Log& operator=(Log const&) = delete;
        Log(Log&&) = delete;
        Log& operator=(Log&&) = delete;

        const std::shared_ptr<spdlog::logger>& GetCoreLogger()
        {
            return m_CoreLogger;
        }
        const std::shared_ptr<spdlog::logger>& GetClientLogger()
        {
            return m_ClientLogger;
        }
        const std::shared_ptr<spdlog::logger>& GetEditorConsoleLogger()
        {
            return m_EditorConsoleLogger;
        }

        // Thread-safe readers: lock-free snapshot
        bool HasTag(const std::string& tag) const;
        std::unordered_map<std::string, TagDetails> GetEnabledTags() const; // returns copy

        // Writers: apply copy-on-write and atomic swap
        void SetTagEnabled(const std::string& tag, bool enabled, Level level = Level::Trace);
        void RemoveTag(const std::string& tag);
        void ClearAllTags();

        void SetDefaultTagSettings();

        // Crash reporting: retrieve the last N formatted log messages from the ringbuffer
        [[nodiscard]] std::vector<std::string> GetRecentLogMessages(std::size_t count = 0) const;

        template<typename... Args>
        static void PrintMessage(Log::Type type, Log::Level level, const std::string& format, Args&&... args);

        template<typename... Args>
        static void PrintMessageTag(Log::Type type, Log::Level level, std::string_view tag, const std::string& format, Args&&... args);

        static void PrintMessageTag(Log::Type type, Log::Level level, std::string_view tag, std::string_view message);

        template<typename... Args>
        static void PrintAssertMessage(Log::Type type, std::string_view prefix, const std::string& message, Args&&... args);

        static void PrintAssertMessage(Log::Type type, std::string_view prefix);

        // Enum utils
        static const char* LevelToString(Level level)
        {
            switch (level)
            {
                case Level::Trace:
                    return "Trace";
                case Level::Info:
                    return "Info";
                case Level::Warn:
                    return "Warn";
                case Level::Error:
                    return "Error";
                case Level::Fatal:
                    return "Fatal";
            }
            return "";
        }
        static Level LevelFromString(std::string_view string)
        {
            if (string == "Trace")
                return Level::Trace;
            if (string == "Info")
                return Level::Info;
            if (string == "Warn")
                return Level::Warn;
            if (string == "Error")
                return Level::Error;
            if (string == "Fatal")
                return Level::Fatal;

            return Level::Trace;
        }

      private:
        Log();
        ~Log();

        static inline std::atomic<bool> s_Initialized{ false };

        // lock-free tag map: readers take a snapshot shared_ptr, writers copy-and-swap
        using TagMap = std::unordered_map<std::string, TagDetails>;
        TagDetails GetTagDetails(std::string_view tag) const;

      private:
        std::shared_ptr<spdlog::logger> m_CoreLogger;
        std::shared_ptr<spdlog::logger> m_ClientLogger;
        std::shared_ptr<spdlog::logger> m_EditorConsoleLogger;
        std::shared_ptr<spdlog::sinks::ringbuffer_sink<std::mutex>> m_RingbufferSink;

        // lock-free storage
        mutable std::atomic<std::shared_ptr<TagMap>> m_Tags{ nullptr };
        // keep a copy of defaults if you need reset functionality
        std::unordered_map<std::string, TagDetails> m_DefaultTagDetails;
    };

    // Template implementations
    template<typename... Args>
    void Log::PrintMessage(Log::Type type, Log::Level level, const std::string& format, Args&&... args)
    {
        auto& log = Get();
        if (const auto detail = log.GetTagDetails(""); detail.Enabled && detail.LevelFilter <= level)
        {
            auto& logger = (type == Type::Core) ? log.GetCoreLogger() : log.GetClientLogger();
            switch (level)
            {
                case Level::Trace:
                    logger->trace(fmt::runtime(format), std::forward<Args>(args)...);
                    break;
                case Level::Info:
                    logger->info(fmt::runtime(format), std::forward<Args>(args)...);
                    break;
                case Level::Warn:
                    logger->warn(fmt::runtime(format), std::forward<Args>(args)...);
                    break;
                case Level::Error:
                    logger->error(fmt::runtime(format), std::forward<Args>(args)...);
                    break;
                case Level::Fatal:
                    logger->critical(fmt::runtime(format), std::forward<Args>(args)...);
                    break;
            }
        }
    }

    template<typename... Args>
    void Log::PrintMessageTag(Log::Type type, Log::Level level, std::string_view tag, const std::string& format, Args&&... args)
    {
        auto& log = Get();
        if (const auto detail = log.GetTagDetails(tag); detail.Enabled && detail.LevelFilter <= level)
        {
            auto& logger = (type == Type::Core) ? log.GetCoreLogger() : log.GetClientLogger();

            // Build tagged format (simple and clear)
            const std::string tagged_format = "[{}] " + format;

            switch (level)
            {
                case Level::Trace:
                    logger->trace(fmt::runtime(tagged_format), tag, std::forward<Args>(args)...);
                    break;
                case Level::Info:
                    logger->info(fmt::runtime(tagged_format), tag, std::forward<Args>(args)...);
                    break;
                case Level::Warn:
                    logger->warn(fmt::runtime(tagged_format), tag, std::forward<Args>(args)...);
                    break;
                case Level::Error:
                    logger->error(fmt::runtime(tagged_format), tag, std::forward<Args>(args)...);
                    break;
                case Level::Fatal:
                    logger->critical(fmt::runtime(tagged_format), tag, std::forward<Args>(args)...);
                    break;
            }
        }
    }

    inline void Log::PrintMessageTag(Log::Type type, Log::Level level, std::string_view tag, std::string_view message)
    {
        auto& log = Get();
        if (const auto detail = log.GetTagDetails(tag); detail.Enabled && detail.LevelFilter <= level)
        {
            auto& logger = (type == Type::Core) ? log.GetCoreLogger() : log.GetClientLogger();
            switch (level)
            {
                case Level::Trace:
                    logger->trace("[{0}] {1}", tag, message);
                    break;
                case Level::Info:
                    logger->info("[{0}] {1}", tag, message);
                    break;
                case Level::Warn:
                    logger->warn("[{0}] {1}", tag, message);
                    break;
                case Level::Error:
                    logger->error("[{0}] {1}", tag, message);
                    break;
                case Level::Fatal:
                    logger->critical("[{0}] {1}", tag, message);
                    break;
            }
        }
    }

    template<typename... Args>
    void Log::PrintAssertMessage(Log::Type type, std::string_view prefix, const std::string& message, Args&&... args)
    {
        auto& log = Get();
        auto& logger = (type == Type::Core) ? log.GetCoreLogger() : log.GetClientLogger();
        std::string formatted = spdlog::fmt_lib::format(fmt::runtime(message), std::forward<Args>(args)...);
        std::string full = spdlog::fmt_lib::format("{}: {}", prefix, formatted);
        logger->error(full);

#if OLO_ASSERT_MESSAGE_BOX
        ShowAssertMessageBox(full.c_str());
#endif
    }

    inline void Log::PrintAssertMessage(Log::Type type, std::string_view prefix)
    {
        auto& log = Get();
        auto& logger = (type == Type::Core) ? log.GetCoreLogger() : log.GetClientLogger();
        std::string full = spdlog::fmt_lib::format("{}: Assertion Failed", prefix);
        logger->error(full);
#if OLO_ASSERT_MESSAGE_BOX
        ShowAssertMessageBox(full.c_str());
#endif
    }
} // namespace OloEngine

// glm stream operators
template<typename OStream, glm::length_t L, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, const glm::vec<L, T, Q>& vector)
{
    os << glm::to_string(vector);
    return os;
}

template<typename OStream, glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, const glm::mat<C, R, T, Q>& matrix)
{
    os << glm::to_string(matrix);
    return os;
}

template<typename OStream, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, glm::qua<T, Q> quaternion)
{
    os << glm::to_string(quaternion);
    return os;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tagged logs (prefer these!)                                                                                      //
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Core logging
#define OLO_CORE_TRACE_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Trace, tag, __VA_ARGS__)
#define OLO_CORE_INFO_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Info, tag, __VA_ARGS__)
#define OLO_CORE_WARN_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Warn, tag, __VA_ARGS__)
#define OLO_CORE_ERROR_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Error, tag, __VA_ARGS__)
#define OLO_CORE_FATAL_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Fatal, tag, __VA_ARGS__)

// Client logging
#define OLO_TRACE_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Trace, tag, __VA_ARGS__)
#define OLO_INFO_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Info, tag, __VA_ARGS__)
#define OLO_WARN_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Warn, tag, __VA_ARGS__)
#define OLO_ERROR_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Error, tag, __VA_ARGS__)
#define OLO_FATAL_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Fatal, tag, __VA_ARGS__)

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Core Logging
#define OLO_CORE_TRACE(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Trace, __VA_ARGS__)
#define OLO_CORE_INFO(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Info, __VA_ARGS__)
#define OLO_CORE_WARN(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Warn, __VA_ARGS__)
#define OLO_CORE_ERROR(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Error, __VA_ARGS__)
#define OLO_CORE_FATAL(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Fatal, __VA_ARGS__)
#define OLO_CORE_CRITICAL(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Fatal, __VA_ARGS__)

// Client Logging
#define OLO_TRACE(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Trace, __VA_ARGS__)
#define OLO_INFO(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Info, __VA_ARGS__)
#define OLO_WARN(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Warn, __VA_ARGS__)
#define OLO_ERROR(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Error, __VA_ARGS__)
#define OLO_FATAL(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Fatal, __VA_ARGS__)
#define OLO_CRITICAL(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Fatal, __VA_ARGS__)

// Editor Console Logging Macros
#define OLO_CONSOLE_LOG_TRACE(...) ::OloEngine::Log::Get().GetEditorConsoleLogger()->trace(__VA_ARGS__)
#define OLO_CONSOLE_LOG_INFO(...) ::OloEngine::Log::Get().GetEditorConsoleLogger()->info(__VA_ARGS__)
#define OLO_CONSOLE_LOG_WARN(...) ::OloEngine::Log::Get().GetEditorConsoleLogger()->warn(__VA_ARGS__)
#define OLO_CONSOLE_LOG_ERROR(...) ::OloEngine::Log::Get().GetEditorConsoleLogger()->error(__VA_ARGS__)
#define OLO_CONSOLE_LOG_FATAL(...) ::OloEngine::Log::Get().GetEditorConsoleLogger()->critical(__VA_ARGS__)
