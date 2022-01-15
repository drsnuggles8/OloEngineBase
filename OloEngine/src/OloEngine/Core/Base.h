#pragma once

#include <memory>

#include "OloEngine/Core/PlatformDetection.h"

#ifdef OLO_DEBUG
	#if defined(OLO_PLATFORM_WINDOWS)
		#define OLO_DEBUGBREAK() __debugbreak()
	#elif defined(OLO_PLATFORM_LINUX)
		#include <signal.h>
		#define OLO_DEBUGBREAK() raise(SIGTRAP)
	#else
		#error "Platform doesn't support debugbreak yet!"
	#endif
	#define OLO_ENABLE_ASSERTS
#else
	#define OLO_DEBUGBREAK()
#endif

// TODO: Make this macro able to take in no arguments except condition
#ifdef OLO_ENABLE_ASSERTS
	#define OLO_ASSERT(x, ...) { if(!(x)) { OLO_ERROR("Assertion Failed: {0}", __VA_ARGS__); OLO_DEBUGBREAK(); } }
	#define OLO_CORE_ASSERT(x, ...) { if(!(x)) { OLO_CORE_ERROR("Assertion Failed: {0}", __VA_ARGS__); OLO_DEBUGBREAK(); } }
#else
	#define OLO_ASSERT(x, ...)
	#define OLO_CORE_ASSERT(x, ...)
#endif

#define BIT(x) ((1) << (x))

#define OLO_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

namespace OloEngine {

	template<typename T>
	using Scope = std::unique_ptr<T>;
	template<typename T, typename ... Args>
	constexpr Scope<T> CreateScope(Args&& ... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	template<typename T>
	using Ref = std::shared_ptr<T>;
	template<typename T, typename ... Args>
	constexpr Ref<T> CreateRef(Args&& ... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}

}
