#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include <filesystem>

#ifdef OLO_DEBUG
	#define OLO_ENABLE_ASSERTS
#endif

#define OLO_ENABLE_VERIFY

#ifdef OLO_ENABLE_ASSERTS
	// Use __VA_OPT__ only if compiler and C++ standard support it
	#if __cplusplus < 202002L
		#define OLO_CORE_ASSERT_MESSAGE_INTERNAL(...)  ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Core, "Assertion Failed", ##__VA_ARGS__)
		#define OLO_ASSERT_MESSAGE_INTERNAL(...)  ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Client, "Assertion Failed", ##__VA_ARGS__)
	#else
		#define OLO_CORE_ASSERT_MESSAGE_INTERNAL(...)  ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Core, "Assertion Failed" __VA_OPT__(,) __VA_ARGS__)
		#define OLO_ASSERT_MESSAGE_INTERNAL(...)  ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Client, "Assertion Failed" __VA_OPT__(,) __VA_ARGS__)
	#endif

	#define OLO_CORE_ASSERT(condition, ...) do { if(!(condition)) { OLO_CORE_ASSERT_MESSAGE_INTERNAL(__VA_ARGS__); OLO_DEBUGBREAK(); } } while(0)
	#define OLO_ASSERT(condition, ...) do { if(!(condition)) { OLO_ASSERT_MESSAGE_INTERNAL(__VA_ARGS__); OLO_DEBUGBREAK(); } } while(0)
#else
	#define OLO_CORE_ASSERT(condition, ...) ((void) (condition))
	#define OLO_ASSERT(condition, ...) ((void) (condition))
#endif

#ifdef OLO_ENABLE_VERIFY
	// Use __VA_OPT__ only if compiler and C++ standard support it
	#if __cplusplus < 202002L
		#define OLO_CORE_VERIFY_MESSAGE_INTERNAL(...)  ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Core, "Verify Failed", ##__VA_ARGS__)
		#define OLO_VERIFY_MESSAGE_INTERNAL(...)  ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Client, "Verify Failed", ##__VA_ARGS__)
	#else
		#define OLO_CORE_VERIFY_MESSAGE_INTERNAL(...)  ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Core, "Verify Failed" __VA_OPT__(,) __VA_ARGS__)
		#define OLO_VERIFY_MESSAGE_INTERNAL(...)  ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Client, "Verify Failed" __VA_OPT__(,) __VA_ARGS__)
	#endif

	#define OLO_CORE_VERIFY(condition, ...) do { if(!(condition)) { OLO_CORE_VERIFY_MESSAGE_INTERNAL(__VA_ARGS__); OLO_DEBUGBREAK(); } } while(0)
	#define OLO_VERIFY(condition, ...) do { if(!(condition)) { OLO_VERIFY_MESSAGE_INTERNAL(__VA_ARGS__); OLO_DEBUGBREAK(); } } while(0)
#else
	#define OLO_ASSERT(...)
	#define OLO_CORE_ASSERT(...)
#endif
