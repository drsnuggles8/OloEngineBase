#pragma once

#include "Core.h"
#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace OloEngine {
	class OLO_API Log
	{
	public:
		static void Init();

		inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }
	private:
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
	};
};

// Core log macros
#define OLO_CORE_TRACE(...)   ::OloEngine::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define OLO_CORE_INFO(...)    ::OloEngine::Log::GetCoreLogger()->info(__VA_ARGS__)
#define OLO_CORE_WARN(...)    ::OloEngine::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define OLO_CORE_ERROR(...)   ::OloEngine::Log::GetCoreLogger()->error(__VA_ARGS__)
#define OLO_CORE_FATAL(...)   ::OloEngine::Log::GetCoreLogger()->critical(__VA_ARGS__)

// Client log macros
#define OLO_TRACE(...)        ::OloEngine::Log::GetClientLogger()->trace(__VA_ARGS__)
#define OLO_INFO(...)		  ::OloEngine::Log::GetClientLogger()->info(__VA_ARGS__)
#define OLO_WARN(...)		  ::OloEngine::Log::GetClientLogger()->warn(__VA_ARGS__)
#define OLO_ERROR(...)		  ::OloEngine::Log::GetClientLogger()->error(__VA_ARGS__)
#define OLO_FATAL(...)		  ::OloEngine::Log::GetClientLogger()->critical(__VA_ARGS__)