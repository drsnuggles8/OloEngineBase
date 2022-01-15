#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"

#ifdef OLO_ENABLE_ASSERTS

	namespace OloEngine::Assert
	{
		// Returns the simple file name rather than full path as suggested by LovelySanta
		constexpr const char* CurrentFileName(const char* path)
		{
			const char* file = path;
			while (*path)
			{
				if (*path == '/' || *path == '\\')
					file = ++path;
				else
					path++;
			}
			return file;
		}
	}

	// Alteratively we could use the same "default" message for both "WITH_MSG" and "NO_MSG" and
	// provide support for custom formatting by concatenating the formatting string instead of having the format inside the default message
	#define OLO_INTERNAL_ASSERT_IMPL(type, check, msg, ...) { if(!(check)) { OLO##type##ERROR(msg, __VA_ARGS__); OLO_DEBUGBREAK(); } }
	#define OLO_INTERNAL_ASSERT_WITH_MSG(type, check, ...) OLO_INTERNAL_ASSERT_IMPL(type, check, "Assertion failed: {0}", __VA_ARGS__)
	#define OLO_INTERNAL_ASSERT_NO_MSG(type, check) OLO_INTERNAL_ASSERT_IMPL(type, check, "Assertion '{0}' failed at {1}:{2}", OLO_STRINGIFY_MACRO(check), ::OloEngine::Assert::CurrentFileName(__FILE__), __LINE__)

	#define OLO_INTERNAL_ASSERT_GET_MACRO_NAME(arg1, arg2, macro, ...) macro
	#define OLO_INTERNAL_ASSERT_GET_MACRO(...) OLO_EXPAND_MACRO( OLO_INTERNAL_ASSERT_GET_MACRO_NAME(__VA_ARGS__, OLO_INTERNAL_ASSERT_WITH_MSG, OLO_INTERNAL_ASSERT_NO_MSG) )

	// Currently accepts at least the condition and one additional parameter (the message) being optional
	#define OLO_ASSERT(...) OLO_EXPAND_MACRO( OLO_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_, __VA_ARGS__) )
	#define OLO_CORE_ASSERT(...) OLO_EXPAND_MACRO( OLO_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_CORE_, __VA_ARGS__) )
#else
	#define OLO_ASSERT(...)
	#define OLO_CORE_ASSERT(...)
#endif
