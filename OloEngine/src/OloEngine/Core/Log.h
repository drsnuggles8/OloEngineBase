#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
#include <memory>

#include "OloEngine/Core/Base.h"

// This ignores all warnings raised inside the following external headers
#pragma warning(push, 0)
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#pragma warning(pop)

namespace OloEngine
{
	class Log
	{
	public:
		static void Init();

		[[nodiscard("Store this!")]] static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		[[nodiscard("Store this!")]] static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }
	private:
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
	};
}

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

// Core log macros
#define OLO_CORE_TRACE(...)   ::OloEngine::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define OLO_CORE_INFO(...)    ::OloEngine::Log::GetCoreLogger()->info(__VA_ARGS__)
#define OLO_CORE_WARN(...)    ::OloEngine::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define OLO_CORE_ERROR(...)   ::OloEngine::Log::GetCoreLogger()->error(__VA_ARGS__)
#define OLO_CORE_CRITICAL(...)::OloEngine::Log::GetCoreLogger()->critical(__VA_ARGS__)

// Client log macros
#define OLO_TRACE(...)        ::OloEngine::Log::GetClientLogger()->trace(__VA_ARGS__)
#define OLO_INFO(...)		  ::OloEngine::Log::GetClientLogger()->info(__VA_ARGS__)
#define OLO_WARN(...)		  ::OloEngine::Log::GetClientLogger()->warn(__VA_ARGS__)
#define OLO_ERROR(...)		  ::OloEngine::Log::GetClientLogger()->error(__VA_ARGS__)
#define OLO_CRITICAL(...)	  ::OloEngine::Log::GetClientLogger()->critical(__VA_ARGS__)
