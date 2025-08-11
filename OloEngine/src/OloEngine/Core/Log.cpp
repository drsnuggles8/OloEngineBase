#include "OloEnginePCH.h"
#include "OloEngine/Core/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace OloEngine
{
	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;
	std::shared_ptr<spdlog::logger> Log::s_EditorConsoleLogger;
	std::map<std::string, Log::TagDetails> Log::s_DefaultTagDetails;

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
	}

	void Log::SetDefaultTagSettings()
	{
		std::unique_lock<std::shared_mutex> lock(s_TagMutex);
		
		// Default tag for untagged messages
		s_EnabledTags[""] = { true, Level::Trace };
		
		// Set up some common tags with their default settings
		s_EnabledTags["Renderer"] = { true, Level::Trace };
		s_EnabledTags["Core"] = { true, Level::Trace };
		s_EnabledTags["Asset"] = { true, Level::Trace };
		s_EnabledTags["Scene"] = { true, Level::Trace };
		s_EnabledTags["Input"] = { true, Level::Info };
		s_EnabledTags["Event"] = { true, Level::Info };
		s_EnabledTags["Script"] = { true, Level::Trace };
		s_EnabledTags["Audio"] = { true, Level::Trace };
		s_EnabledTags["Physics"] = { true, Level::Trace };
		s_EnabledTags["UI"] = { true, Level::Trace };
		s_EnabledTags["FileSystem"] = { true, Level::Info };
		s_EnabledTags["Memory"] = { true, Level::Warn };
		s_EnabledTags["Performance"] = { true, Level::Info };

		// Store defaults for potential reset functionality
		s_DefaultTagDetails = s_EnabledTags;
	}

	Log::TagDetails Log::GetTagDetails(std::string_view tag)
	{
		{
			std::shared_lock<std::shared_mutex> shared_lock(s_TagMutex);
			auto cache_it = s_TagCache.find(tag);
			if (cache_it != s_TagCache.end())
			{
				return *cache_it->second; // Safe copy while holding shared lock
			}
		}

		// Cache miss - need exclusive lock for potential mutations
		std::unique_lock<std::shared_mutex> exclusive_lock(s_TagMutex);
		
		// Double-check cache after acquiring exclusive lock (another thread might have added it)
		auto cache_it = s_TagCache.find(tag);
		if (cache_it != s_TagCache.end())
		{
			return *cache_it->second; // Safe copy while holding exclusive lock
		}

		// Not in cache, do map lookup (may require string conversion)
		auto it = s_EnabledTags.find(std::string(tag));
		if (it == s_EnabledTags.end())
		{
			// Tag not found, create with default settings
			auto [inserted_it, success] = s_EnabledTags.emplace(std::string(tag), TagDetails{});
			it = inserted_it;
		}

		// Cache the result using the stable string from the map as key
		s_TagCache[std::string_view(it->first)] = &it->second;
		return it->second; // Safe copy while holding exclusive lock
	}

	void Log::SetTagEnabled(const std::string& tag, bool enabled, Level level)
	{
		std::unique_lock<std::shared_mutex> lock(s_TagMutex);
		
		auto it = s_EnabledTags.find(tag);
		if (it != s_EnabledTags.end())
		{
			it->second.Enabled = enabled;
			it->second.LevelFilter = level;
		}
		else
		{
			s_EnabledTags[tag] = { enabled, level };
		}
		
		// Clear cache to ensure consistency
		s_TagCache.clear();
	}

	void Log::RemoveTag(const std::string& tag)
	{
		std::unique_lock<std::shared_mutex> lock(s_TagMutex);
		
		s_EnabledTags.erase(tag);
		
		// Clear cache to ensure consistency
		s_TagCache.clear();
	}

	void Log::ClearAllTags()
	{
		std::unique_lock<std::shared_mutex> lock(s_TagMutex);
		
		s_EnabledTags.clear();
		s_TagCache.clear();
	}
}
