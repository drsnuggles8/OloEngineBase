#pragma once

#ifdef OLO_PLATFORM_WINDOWS
#ifdef OLO_BUILD_DLL
#define OLO_API __declspec(dllexport)
#else
#define OLO_API __declspec(dllimport)
#endif
#else
#error OloEngine only supports Windows!
#endif

#ifdef OLO_DEBUG
#define OLO_ENABLE_ASSERTS
#endif

#ifdef OLO_ENABLE_ASSERTS
#define OLO_ASSERT(x, ...) { if (!(x)) { OLO_ERROR("Assertion Failed: {0}", __VA_ARGS__); __debugbreak(); } }
#define OLO_CORE_ASSERT(x, ...) { if (!(x)) { OLO_CORE_ERROR("Assertion Failed: {0}", __VA_ARGS__); __debugbreak(); } }
#else
#define OLO_ASSERT(x, ...)
#define OLO_CORE_ASSERT(x, ...)
#endif

#define BIT(x) (1 << x)

#define OLO_BIND_EVENT_FN(fn) std::bind(&fn, this, std::placeholders::_1)