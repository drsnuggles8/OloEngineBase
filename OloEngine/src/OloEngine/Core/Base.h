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

#define OLO_EXPAND_MACRO(x) x
#define OLO_STRINGIFY_MACRO(x) #x

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

#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Assert.h"
