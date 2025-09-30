#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <cstdint>

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

// Bit manipulation functions
// 
// Usage examples:
//   enum TinyFlags : u8 { FlagA = OloBit8(0), FlagB = OloBit8(1) };   // For bits 0-7 (backward compatible)
//   enum SmallFlags : u16 { FlagA = OloBit16(0), FlagB = OloBit16(1) }; // For bits 0-15 (backward compatible)
//   enum Flags : u32 { FlagA = OloBit32(0), FlagB = OloBit32(1) };   // For bits 0-31 (backward compatible)
//   enum LargeFlags : u64 { BigFlag = OloBit64(35) };                // For bits 32-63 (backward compatible)
//   enum ModernFlags : u32 { Flag = OloBit<u32>(5) };               // Modern template usage
//   auto mask = OloEngine::BitMask<u32>(5);                          // Type-safe u32 mask (runtime)
//   auto bigMask = OloEngine::BitMask<u64>(45);                      // Type-safe u64 mask (runtime)
//   constexpr auto cmask = OloEngine::BitMaskConstexpr<u32>(5);      // Compile-time constexpr safe
//
// Bounds-checked bit manipulation template - enforces compile-time and runtime safety
template<typename T>
constexpr T OloBit(int x)
{
    constexpr int max_bits = sizeof(T) * 8;
    if (x < 0 || x >= max_bits)
    {
        throw std::out_of_range("Bit index out of range for bit manipulation");
    }
    return T(1) << static_cast<unsigned>(x);
}

// Backward compatibility aliases for existing code
constexpr std::uint8_t OloBit8(int x) { return OloBit<std::uint8_t>(x); }
constexpr std::uint16_t OloBit16(int x) { return OloBit<std::uint16_t>(x); }
constexpr std::uint32_t OloBit32(int x) { return OloBit<std::uint32_t>(x); }
constexpr std::uint64_t OloBit64(int x) { return OloBit<std::uint64_t>(x); }


namespace OloEngine
{
	// Type-safe bit mask generator for compile-time usage (C++20 constexpr compatible)
	template<typename T>
	constexpr T BitMaskConstexpr(unsigned idx) 
	{
		static_assert(std::is_integral_v<T>, "BitMaskConstexpr() requires integral types");
		static_assert(sizeof(T) <= 8, "BitMaskConstexpr() supports types up to 8 bytes (64-bit)");
		
		// Return 0 for out-of-range indices to avoid throwing in constexpr context
		return (idx >= sizeof(T) * 8) ? T(0) : T(1) << idx;
	}

	// Type-safe bit mask generator with runtime bounds checking
	template<typename T>
	T BitMask(unsigned idx) 
	{
		static_assert(std::is_integral_v<T>, "BitMask() requires integral types");
		static_assert(sizeof(T) <= 8, "BitMask() supports types up to 8 bytes (64-bit)");
		
		// Runtime bounds check with exception for out-of-range indices
		if (idx >= sizeof(T) * 8) {
			throw std::out_of_range("Bit index exceeds type width");
		}
		
		return T(1) << idx;
	}

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

//==============================================================================
/// Flag utilities for state tracking

/**
 * Thread-safe atomic flag for inter-thread communication
 * Based on Hazel's AtomicFlag implementation
 */
struct AtomicFlag
{
	OLO_FINLINE void SetDirty() { m_Flag.clear(); }
	OLO_FINLINE bool CheckAndResetIfDirty() { return !m_Flag.test_and_set(); }

	explicit AtomicFlag() noexcept { m_Flag.test_and_set(); }
	AtomicFlag(const AtomicFlag&) = delete;
	AtomicFlag& operator=(const AtomicFlag&) = delete;
	AtomicFlag(AtomicFlag&&) = delete;
	AtomicFlag& operator=(AtomicFlag&&) = delete;

private:
	std::atomic_flag m_Flag{};
};

/**
 * Simple flag for tracking dirty state (single-threaded)
 * Based on Hazel's Flag implementation
 */
struct Flag
{
	OLO_FINLINE void SetDirty() noexcept { m_Flag = true; }
	OLO_FINLINE bool CheckAndResetIfDirty() noexcept
	{
		if (m_Flag)
		{
			m_Flag = false;
			return true;
		}
		else
		{
			return false;
		}
	}

	OLO_FINLINE bool IsDirty() const noexcept
	{
		return m_Flag;
	}
	

private:
	bool m_Flag = false;
};

#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Assert.h"
