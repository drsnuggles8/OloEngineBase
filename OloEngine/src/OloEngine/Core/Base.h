#pragma once

#include <memory>

#include "OloEngine/Core/PlatformDetection.h"

// Compiler Detection ////////////////////////////////////////////////////
// Use explicit 0/1 numeric macros for safer conditional checks

#if defined(__clang__)
	// Clang (including clang-cl on Windows)
	#define OLO_COMPILER_CLANG 1
	#define OLO_COMPILER_GCC 0
	#if defined(_MSC_VER)
		// clang-cl: Clang with MSVC-compatible interface
		#define OLO_COMPILER_MSVC 1
		#define OLO_COMPILER_CLANG_CL 1
	#else
		// Regular clang
		#define OLO_COMPILER_MSVC 0
		#define OLO_COMPILER_CLANG_CL 0
	#endif
	#define OLO_COMPILER_UNKNOWN 0
#elif defined(__GNUC__) || defined(__GNUG__)
	// GCC
	#define OLO_COMPILER_CLANG 0
	#define OLO_COMPILER_GCC 1
	#define OLO_COMPILER_MSVC 0
	#define OLO_COMPILER_CLANG_CL 0
	#define OLO_COMPILER_UNKNOWN 0
#elif defined(_MSC_VER)
	// MSVC
	#define OLO_COMPILER_CLANG 0
	#define OLO_COMPILER_GCC 0
	#define OLO_COMPILER_MSVC 1
	#define OLO_COMPILER_CLANG_CL 0
	#define OLO_COMPILER_UNKNOWN 0
#else
	// Unknown compiler - fallback
	#define OLO_COMPILER_CLANG 0
	#define OLO_COMPILER_GCC 0
	#define OLO_COMPILER_MSVC 0
	#define OLO_COMPILER_CLANG_CL 0
	#define OLO_COMPILER_UNKNOWN 1
#endif

// Macros ////////////////////////////////////////////////////////////////

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

#if OLO_COMPILER_MSVC
	#define OLO_INLINE                               inline
	#define OLO_FINLINE                              __forceinline
	#define OLO_DISABLE_WARNING(warning_number)      __pragma( warning( disable : warning_number ) )
	#define OLO_CONCAT_OPERATOR(x, y)                x##y
#else
	#define OLO_INLINE                               inline
	#define OLO_FINLINE                              always_inline
	#define OLO_CONCAT_OPERATOR(x, y)                x y
#endif // MSVC


template<typename T>
constexpr auto ArraySize(T array) { return ( sizeof(array)/sizeof((array)[0]) ); }
#define OLO_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

#define OLO_EXPAND_MACRO(x) x
#define OLO_STRINGIFY_MACRO(x) #x
#define OLO_MAKESTRING(x) OLO_STRINGIFY_MACRO(x)
#define OLO_CONCAT(x, y)                         OLO_CONCAT_OPERATOR(x, y)
#define OLO_LINE_STRING                          OLO_MAKESTRING( __LINE__ )
#define OLO_FILELINE(MESSAGE)                    __FILE__ "(" OLO_LINE_STRING ") : " MESSAGE

// Unique names
#define OLO_UNIQUE_SUFFIX(PARAM) OLO_CONCAT(PARAM, __LINE__ )

#define BIT(x) ((1) << (x))


namespace OloEngine
{
	template<typename T>
	using Scope = std::unique_ptr<T>;
	template<typename T, typename ... Args>
	constexpr Scope<T> CreateScope(Args&& ... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

}

// Native types typedefs /////////////////////////////////////////////////
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using f32 = float;
using f64 = double;

using sizet = size_t;

static const u64 u64_max = UINT64_MAX;
static const i64 i64_max = INT64_MAX;
static const u32 u32_max = UINT32_MAX;
static const i32 i32_max = INT32_MAX;
static const u16 u16_max = UINT16_MAX;
static const i16 i16_max = INT16_MAX;
static const u8 u8_max = UINT8_MAX;
static const i8 i8_max = INT8_MAX;

#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Assert.h"
