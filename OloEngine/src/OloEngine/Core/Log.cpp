#include "OloEnginePCH.h"
#include "OloEngine/Core/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace OloEngine
{
    std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
    std::shared_ptr<spdlog::logger> Log::s_ClientLogger;
    std::shared_ptr<spdlog::logger> Log::s_EditorConsoleLogger;
    std::unordered_map<std::string, Log::TagDetails> Log::s_DefaultTagDetails;
    std::atomic<std::shared_ptr<Log::TagMap>> Log::s_Tags{ nullptr };

    void Log::Init()
    {
        std::vector<spdlog::sink_ptr> logSinks;
        logSinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        logSinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("OloEngine.log", true));

        logSinks[0]->set_pattern("%^[%T] %n: %v%$");
        logSinks[1]->set_pattern("[%T] [%l] %n: %v");

        s_CoreLogger = std::make_shared<spdlog::logger>("OloEngine", begin(logSinks), end(logSinks));
        spdlog::register_logger(s_CoreLogger);
        s_CoreLogger->set_level(spdlog::level::trace);
        s_CoreLogger->flush_on(spdlog::level::trace);

        s_ClientLogger = std::make_shared<spdlog::logger>("APP", begin(logSinks), end(logSinks));
        spdlog::register_logger(s_ClientLogger);
        s_ClientLogger->set_level(spdlog::level::trace);
        s_ClientLogger->flush_on(spdlog::level::trace);

        // Editor console logger (for potential future use)
        s_EditorConsoleLogger = std::make_shared<spdlog::logger>("EditorConsole", logSinks[0]);
        spdlog::register_logger(s_EditorConsoleLogger);
        s_EditorConsoleLogger->set_level(spdlog::level::trace);
        s_EditorConsoleLogger->flush_on(spdlog::level::trace);

        SetDefaultTagSettings();
    }

    void Log::Shutdown()
    {
        spdlog::shutdown();
        // release tags explicitly
        s_Tags.store(nullptr, std::memory_order_release);
    }

    void Log::SetDefaultTagSettings()
    {
        auto tags = std::make_shared<TagMap>();

        // Default tag for untagged messages
        (*tags)[""] = { true, Level::Trace };

        // Set up some common tags with their default settings
        (*tags)["Renderer"] = { true, Level::Trace };
        (*tags)["Core"] = { true, Level::Trace };
        (*tags)["Asset"] = { true, Level::Trace };
        (*tags)["Scene"] = { true, Level::Trace };
        (*tags)["Input"] = { true, Level::Info };
        (*tags)["Event"] = { true, Level::Info };
        (*tags)["Script"] = { true, Level::Trace };
        (*tags)["Audio"] = { true, Level::Trace };
        (*tags)["Physics"] = { true, Level::Trace };
        (*tags)["UI"] = { true, Level::Trace };
        (*tags)["FileSystem"] = { true, Level::Info };
        (*tags)["Memory"] = { true, Level::Warn };
        (*tags)["Performance"] = { true, Level::Info };

        // store defaults if needed for resets
        s_DefaultTagDetails = *tags;

        // publish snapshot
        s_Tags.store(tags, std::memory_order_release);
    }

    Log::TagDetails Log::GetTagDetails(std::string_view tag)
    {
        // fast-path: snapshot current tags (no locks)
        auto current = s_Tags.load(std::memory_order_acquire);
        if (current)
        {
            auto it = current->find(std::string(tag));
            if (it != current->end())
                return it->second;
        }

        // not found → perform copy-on-write insert
        // we must loop because another writer may have swapped while we prepared the new map
        for (;;)
        {
            auto snapshot = s_Tags.load(std::memory_order_acquire);
            if (!snapshot)
            {
                // initialise empty snapshot if needed
                auto newMap = std::make_shared<TagMap>();
                auto [insertIt, _] = newMap->emplace(std::string(tag), TagDetails{});
                if (s_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
                {
                    return insertIt->second;
                }
                else
                {
                    continue; // try again with updated snapshot
                }
            }

            // make a copy of the current map, insert, and attempt atomic swap
            auto newMap = std::make_shared<TagMap>(*snapshot);
            auto [insertIt, inserted] = newMap->emplace(std::string(tag), TagDetails{});
            if (!inserted)
            {
                // tag already exists in our snapshot - return existing value
                return insertIt->second;
            }

            if (s_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
            {
                return insertIt->second;
            }
            // else another writer swapped in parallel — retry loop
        }
    }

    bool Log::HasTag(const std::string& tag)
    {
        auto current = s_Tags.load(std::memory_order_acquire);
        if (!current)
            return false;
        return current->find(tag) != current->end();
    }

    std::unordered_map<std::string, Log::TagDetails> Log::GetEnabledTags()
    {
        auto current = s_Tags.load(std::memory_order_acquire);
        if (!current)
            return {};
        return *current; // copy-out
    }

    void Log::SetTagEnabled(const std::string& tag, bool enabled, Level level)
    {
        for (;;)
        {
            auto snapshot = s_Tags.load(std::memory_order_acquire);
            if (!snapshot)
            {
                auto newMap = std::make_shared<TagMap>();
                (*newMap)[tag] = { enabled, level };
                if (s_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
                    return;
                else
                    continue;
            }

            auto newMap = std::make_shared<TagMap>(*snapshot);
            (*newMap)[tag] = { enabled, level };
            if (s_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
                return;
            // else retry
        }
    }

    void Log::RemoveTag(const std::string& tag)
    {
        for (;;)
        {
            auto snapshot = s_Tags.load(std::memory_order_acquire);
            if (!snapshot)
                return;

            auto newMap = std::make_shared<TagMap>(*snapshot);
            newMap->erase(tag);
            if (s_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
                return;
            // else retry
        }
    }

    void Log::ClearAllTags()
    {
        auto newMap = std::make_shared<TagMap>();
        s_Tags.store(newMap, std::memory_order_release);
    }
} // namespace OloEngine
