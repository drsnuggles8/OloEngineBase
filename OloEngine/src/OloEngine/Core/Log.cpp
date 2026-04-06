#include "OloEnginePCH.h"
#include "OloEngine/Core/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#if OLO_ASSERT_MESSAGE_BOX
#ifdef OLO_PLATFORM_WINDOWS
#include <Windows.h>
#endif
#endif

namespace OloEngine
{
#if OLO_ASSERT_MESSAGE_BOX && defined(OLO_PLATFORM_WINDOWS)
    void ShowAssertMessageBox(const char* message)
    {
        MessageBoxA(nullptr, message, "OloEngine Assert", MB_OK | MB_ICONERROR);
    }
#endif

    Log& Log::Get()
    {
        // Intentionally leaked to survive static teardown — other statics
        // may still log via PrintMessage* after normal destruction order.
        static auto* instance = new Log();
        return *instance;
    }

    Log::Log()
    {
        // Create the ringbuffer sink (keeps last 200 messages for crash reports)
        m_RingbufferSink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(200);
        m_RingbufferSink->set_pattern("[%T] [%l] %n: %v");

        std::vector<spdlog::sink_ptr> logSinks;
        logSinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        logSinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("OloEngine.log", true));
        logSinks.emplace_back(m_RingbufferSink);

        logSinks[0]->set_pattern("%^[%T] %n: %v%$");
        logSinks[1]->set_pattern("[%T] [%l] %n: %v");

        m_CoreLogger = std::make_shared<spdlog::logger>("OloEngine", begin(logSinks), end(logSinks));
        spdlog::register_logger(m_CoreLogger);
        m_CoreLogger->set_level(spdlog::level::trace);
        m_CoreLogger->flush_on(spdlog::level::trace);

        m_ClientLogger = std::make_shared<spdlog::logger>("APP", begin(logSinks), end(logSinks));
        spdlog::register_logger(m_ClientLogger);
        m_ClientLogger->set_level(spdlog::level::trace);
        m_ClientLogger->flush_on(spdlog::level::trace);

        // Editor console logger — same sinks as core/client so messages
        // reach the file log and ringbuffer (crash reports) too.
        m_EditorConsoleLogger = std::make_shared<spdlog::logger>("EditorConsole", begin(logSinks), end(logSinks));
        spdlog::register_logger(m_EditorConsoleLogger);
        m_EditorConsoleLogger->set_level(spdlog::level::trace);
        m_EditorConsoleLogger->flush_on(spdlog::level::trace);

        SetDefaultTagSettings();

        s_Initialized.store(true, std::memory_order_release);
    }

    Log::~Log()
    {
        s_Initialized.store(false, std::memory_order_relaxed);

        // Intentionally do NOT call spdlog::shutdown() here.
        // This destructor runs during static teardown, and other statics
        // may still log via PrintMessage* after this point.  Destroying
        // the process-wide spdlog registry would cause use-after-free.
        m_RingbufferSink.reset();
        // release tags explicitly
        m_Tags.store(nullptr, std::memory_order_release);
    }

    std::vector<std::string> Log::GetRecentLogMessages(std::size_t const count) const
    {
        if (m_RingbufferSink)
        {
            return m_RingbufferSink->last_formatted(count);
        }
        return {};
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
        m_DefaultTagDetails = *tags;

        // publish snapshot
        m_Tags.store(tags, std::memory_order_release);
    }

    Log::TagDetails Log::GetTagDetails(std::string_view tag) const
    {
        // fast-path: snapshot current tags (no locks)
        auto current = m_Tags.load(std::memory_order_acquire);
        if (current)
        {
            if (auto it = current->find(std::string(tag)); it != current->end())
            {
                return it->second;
            }
        }

        // not found → perform copy-on-write insert
        // we must loop because another writer may have swapped while we prepared the new map
        for (;;)
        {
            auto snapshot = m_Tags.load(std::memory_order_acquire);
            if (!snapshot)
            {
                // initialise empty snapshot if needed
                auto newMap = std::make_shared<TagMap>();
                auto [insertIt, _] = newMap->emplace(std::string(tag), TagDetails{});
                if (m_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
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

            if (m_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
            {
                return insertIt->second;
            }
            // else another writer swapped in parallel — retry loop
        }
    }

    bool Log::HasTag(const std::string& tag) const
    {
        auto current = m_Tags.load(std::memory_order_acquire);
        if (!current)
        {
            return false;
        }
        return current->find(tag) != current->end();
    }

    std::unordered_map<std::string, Log::TagDetails> Log::GetEnabledTags() const
    {
        auto current = m_Tags.load(std::memory_order_acquire);
        if (!current)
        {
            return {};
        }
        return *current; // copy-out
    }

    void Log::SetTagEnabled(const std::string& tag, bool enabled, Level level)
    {
        for (;;)
        {
            auto snapshot = m_Tags.load(std::memory_order_acquire);
            if (!snapshot)
            {
                auto newMap = std::make_shared<TagMap>();
                (*newMap)[tag] = { enabled, level };
                if (m_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
                {
                    return;
                }
                else
                {
                    continue;
                }
            }

            auto newMap = std::make_shared<TagMap>(*snapshot);
            (*newMap)[tag] = { enabled, level };
            if (m_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
            {
                return;
            }
            // else retry
        }
    }

    void Log::RemoveTag(const std::string& tag)
    {
        for (;;)
        {
            auto snapshot = m_Tags.load(std::memory_order_acquire);
            if (!snapshot)
            {
                return;
            }

            auto newMap = std::make_shared<TagMap>(*snapshot);
            newMap->erase(tag);
            if (m_Tags.compare_exchange_strong(snapshot, newMap, std::memory_order_release, std::memory_order_acquire))
            {
                return;
            }
            // else retry
        }
    }

    void Log::ClearAllTags()
    {
        auto newMap = std::make_shared<TagMap>();
        m_Tags.store(newMap, std::memory_order_release);
    }
} // namespace OloEngine
