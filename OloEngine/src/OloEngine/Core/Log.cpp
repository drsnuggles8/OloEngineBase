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
}
