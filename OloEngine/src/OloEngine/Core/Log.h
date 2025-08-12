#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
#include <memory>
#include <unordered_map>
#include <string>
#include <string_view>
#include <atomic>

#include "OloEngine/Core/Base.h"

// This ignores all warnings raised inside the following external headers
#pragma warning(push, 0)
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#pragma warning(pop)

#if !defined(OLO_DIST) && defined(OLO_PLATFORM_WINDOWS)
    #define OLO_ASSERT_MESSAGE_BOX 1
#else
    #define OLO_ASSERT_MESSAGE_BOX 0
#endif

#if OLO_ASSERT_MESSAGE_BOX
    #ifdef OLO_PLATFORM_WINDOWS
        #include <Windows.h>
    #endif
#endif

namespace OloEngine
{
    class Log
    {
    public:
        enum class Type : u8
        {
            Core = 0, Client = 1
        };

        enum class Level : u8
        {
            Trace = 0, Info, Warn, Error, Fatal
        };

        struct TagDetails
        {
            bool Enabled = true;
            Level LevelFilter = Level::Trace;
        };

    public:
        static void Init();
        static void Shutdown();

        [[nodiscard("Store this!")]] static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
        [[nodiscard("Store this!")]] static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }
        [[nodiscard("Store this!")]] static std::shared_ptr<spdlog::logger>& GetEditorConsoleLogger() { return s_EditorConsoleLogger; }

        // Thread-safe readers: lock-free snapshot
        static bool HasTag(const std::string& tag);
        static std::unordered_map<std::string, TagDetails> GetEnabledTags(); // returns copy

        // Writers: apply copy-on-write and atomic swap
        static void SetTagEnabled(const std::string& tag, bool enabled, Level level = Level::Trace);
        static void RemoveTag(const std::string& tag);
        static void ClearAllTags();

        static void SetDefaultTagSettings();

        template<typename... Args>
        static void PrintMessage(Log::Type type, Log::Level level, const std::string& format, Args&&... args);

        template<typename... Args>
        static void PrintMessageTag(Log::Type type, Log::Level level, std::string_view tag, const std::string& format, Args&&... args);

        static void PrintMessageTag(Log::Type type, Log::Level level, std::string_view tag, std::string_view message);

        template<typename... Args>
        static void PrintAssertMessage(Log::Type type, std::string_view prefix, const std::string& message, Args&&... args);

        static void PrintAssertMessage(Log::Type type, std::string_view prefix);

    private:
        // lock-free tag map: readers take a snapshot shared_ptr, writers copy-and-swap
        using TagMap = std::unordered_map<std::string, TagDetails>;
        static TagDetails GetTagDetails(std::string_view tag);

    public:
        // Enum utils
        static const char* LevelToString(Level level)
        {
            switch (level)
            {
                case Level::Trace: return "Trace";
                case Level::Info:  return "Info";
                case Level::Warn:  return "Warn";
                case Level::Error: return "Error";
                case Level::Fatal: return "Fatal";
            }
            return "";
        }
        static Level LevelFromString(std::string_view string)
        {
            if (string == "Trace") return Level::Trace;
            if (string == "Info")  return Level::Info;
            if (string == "Warn")  return Level::Warn;
            if (string == "Error") return Level::Error;
            if (string == "Fatal") return Level::Fatal;

            return Level::Trace;
        }

    private:
        static std::shared_ptr<spdlog::logger> s_CoreLogger;
        static std::shared_ptr<spdlog::logger> s_ClientLogger;
        static std::shared_ptr<spdlog::logger> s_EditorConsoleLogger;

        // lock-free storage
        static std::atomic<std::shared_ptr<TagMap>> s_Tags;
        // keep a copy of defaults if you need reset functionality
        static std::unordered_map<std::string, TagDetails> s_DefaultTagDetails;
    };

    // Template implementations
    template<typename... Args>
    void Log::PrintMessage(Log::Type type, Log::Level level, const std::string& format, Args&&... args)
    {
        const auto detail = GetTagDetails("");
        if (detail.Enabled && detail.LevelFilter <= level)
        {
            auto logger = (type == Type::Core) ? GetCoreLogger() : GetClientLogger();
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
        const auto detail = GetTagDetails(tag);
        if (detail.Enabled && detail.LevelFilter <= level)
        {
            auto logger = (type == Type::Core) ? GetCoreLogger() : GetClientLogger();

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
        const auto detail = GetTagDetails(tag);
        if (detail.Enabled && detail.LevelFilter <= level)
        {
            auto logger = (type == Type::Core) ? GetCoreLogger() : GetClientLogger();
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
        auto logger = (type == Type::Core) ? GetCoreLogger() : GetClientLogger();
        const std::string full_format = "{}: " + message;
        logger->error(fmt::runtime(full_format), prefix, std::forward<Args>(args)...);

    #if OLO_ASSERT_MESSAGE_BOX
        std::string formatted = spdlog::fmt_lib::format(fmt::runtime(message), std::forward<Args>(args)...);
        MessageBoxA(nullptr, formatted.c_str(), "OloEngine Assert", MB_OK | MB_ICONERROR);
    #endif
    }

    inline void Log::PrintAssertMessage(Log::Type type, std::string_view prefix)
    {
        auto logger = (type == Type::Core) ? GetCoreLogger() : GetClientLogger();
        logger->error("{0}", prefix);
    #if OLO_ASSERT_MESSAGE_BOX
        MessageBoxA(nullptr, "No message :(", "OloEngine Assert", MB_OK | MB_ICONERROR);
    #endif
    }
}

// glm stream operators
template<typename OStream, glm::length_t L, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, const glm::vec<L, T, Q>& vector)
{
    return os << glm::to_string(vector);
}

template<typename OStream, glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, const glm::mat<C, R, T, Q>& matrix)
{
    return os << glm::to_string(matrix);
}

template<typename OStream, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, glm::qua<T, Q> quaternion)
{
    return os << glm::to_string(quaternion);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tagged logs (prefer these!)                                                                                      //
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Core logging
#define OLO_CORE_TRACE_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Trace, tag, __VA_ARGS__)
#define OLO_CORE_INFO_TAG(tag, ...)  ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Info, tag, __VA_ARGS__)
#define OLO_CORE_WARN_TAG(tag, ...)  ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Warn, tag, __VA_ARGS__)
#define OLO_CORE_ERROR_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Error, tag, __VA_ARGS__)
#define OLO_CORE_FATAL_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Fatal, tag, __VA_ARGS__)

// Client logging
#define OLO_TRACE_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Trace, tag, __VA_ARGS__)
#define OLO_INFO_TAG(tag, ...)  ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Info, tag, __VA_ARGS__)
#define OLO_WARN_TAG(tag, ...)  ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Warn, tag, __VA_ARGS__)
#define OLO_ERROR_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Error, tag, __VA_ARGS__)
#define OLO_FATAL_TAG(tag, ...) ::OloEngine::Log::PrintMessageTag(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Fatal, tag, __VA_ARGS__)

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Core Logging
#define OLO_CORE_TRACE(...)  ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Trace, __VA_ARGS__)
#define OLO_CORE_INFO(...)   ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Info, __VA_ARGS__)
#define OLO_CORE_WARN(...)   ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Warn, __VA_ARGS__)
#define OLO_CORE_ERROR(...)  ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Error, __VA_ARGS__)
#define OLO_CORE_FATAL(...)  ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Fatal, __VA_ARGS__)
#define OLO_CORE_CRITICAL(...) ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Core, ::OloEngine::Log::Level::Fatal, __VA_ARGS__)

// Client Logging
#define OLO_TRACE(...)   ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Trace, __VA_ARGS__)
#define OLO_INFO(...)    ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Info, __VA_ARGS__)
#define OLO_WARN(...)    ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Warn, __VA_ARGS__)
#define OLO_ERROR(...)   ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Error, __VA_ARGS__)
#define OLO_FATAL(...)   ::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Fatal, __VA_ARGS__)
#define OLO_CRITICAL(...)::OloEngine::Log::PrintMessage(::OloEngine::Log::Type::Client, ::OloEngine::Log::Level::Fatal, __VA_ARGS__)

// Editor Console Logging Macros
#define OLO_CONSOLE_LOG_TRACE(...)   OloEngine::Log::GetEditorConsoleLogger()->trace(__VA_ARGS__)
#define OLO_CONSOLE_LOG_INFO(...)    OloEngine::Log::GetEditorConsoleLogger()->info(__VA_ARGS__)
#define OLO_CONSOLE_LOG_WARN(...)    OloEngine::Log::GetEditorConsoleLogger()->warn(__VA_ARGS__)
#define OLO_CONSOLE_LOG_ERROR(...)   OloEngine::Log::GetEditorConsoleLogger()->error(__VA_ARGS__)
#define OLO_CONSOLE_LOG_FATAL(...)   OloEngine::Log::GetEditorConsoleLogger()->critical(__VA_ARGS__)
