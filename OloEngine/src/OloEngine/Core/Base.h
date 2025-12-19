#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
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
#define OLO_INLINE inline
#define OLO_FINLINE __forceinline
#define OLO_NOINLINE __declspec(noinline)
#define OLO_RESTRICT __restrict
#define OLO_LIKELY(x) (x)
#define OLO_UNLIKELY(x) (x)
#define OLO_DISABLE_WARNING(warning_number) __pragma(warning(disable : warning_number))
#define OLO_CONCAT_OPERATOR(x, y) x##y
#define OLO_LIFETIMEBOUND // MSVC doesn't support lifetimebound attribute yet
// Deprecation warning control
#define OLO_DISABLE_DEPRECATION_WARNINGS __pragma(warning(push)) __pragma(warning(disable : 4996))
#define OLO_RESTORE_DEPRECATION_WARNINGS __pragma(warning(pop))
#elif OLO_COMPILER_GCC || OLO_COMPILER_CLANG
#define OLO_INLINE inline
#define OLO_FINLINE inline __attribute__((always_inline))
#define OLO_NOINLINE __attribute__((noinline))
#define OLO_RESTRICT __restrict__
#define OLO_LIKELY(x) __builtin_expect(!!(x), 1)
#define OLO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define OLO_CONCAT_OPERATOR(x, y) x y
#if OLO_COMPILER_CLANG
#define OLO_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define OLO_LIFETIMEBOUND // GCC doesn't support lifetimebound attribute yet
#endif
// Deprecation warning control
#define OLO_DISABLE_DEPRECATION_WARNINGS _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define OLO_RESTORE_DEPRECATION_WARNINGS _Pragma("GCC diagnostic pop")
#else
#define OLO_INLINE inline
#define OLO_FINLINE inline
#define OLO_NOINLINE
#define OLO_RESTRICT
#define OLO_LIKELY(x) (x)
#define OLO_UNLIKELY(x) (x)
#define OLO_CONCAT_OPERATOR(x, y) x y
#define OLO_LIFETIMEBOUND
// Deprecation warning control (no-op fallback)
#define OLO_DISABLE_DEPRECATION_WARNINGS
#define OLO_RESTORE_DEPRECATION_WARNINGS
#endif // MSVC

// Non-copyable macro - deletes copy and move constructors/assignment operators
// Usage: OLO_NONCOPYABLE(ClassName);
#define OLO_NONCOPYABLE(ClassName)                   \
    ClassName(const ClassName&) = delete;            \
    ClassName& operator=(const ClassName&) = delete; \
    ClassName(ClassName&&) = delete;                 \
    ClassName& operator=(ClassName&&) = delete

template<typename T>
constexpr auto ArraySize(T array)
{
    return (sizeof(array) / sizeof((array)[0]));
}
#define OLO_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

#define OLO_EXPAND_MACRO(x) x
#define OLO_STRINGIFY_MACRO(x) #x
#define OLO_MAKESTRING(x) OLO_STRINGIFY_MACRO(x)
#define OLO_CONCAT(x, y) OLO_CONCAT_OPERATOR(x, y)
#define OLO_LINE_STRING OLO_MAKESTRING(__LINE__)
#define OLO_FILELINE(MESSAGE) __FILE__ "(" OLO_LINE_STRING ") : " MESSAGE

// Unique names
#define OLO_UNIQUE_SUFFIX(PARAM) OLO_CONCAT(PARAM, __LINE__)

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
constexpr std::uint8_t OloBit8(int x)
{
    return OloBit<std::uint8_t>(x);
}
constexpr std::uint16_t OloBit16(int x)
{
    return OloBit<std::uint16_t>(x);
}
constexpr std::uint32_t OloBit32(int x)
{
    return OloBit<std::uint32_t>(x);
}
constexpr std::uint64_t OloBit64(int x)
{
    return OloBit<std::uint64_t>(x);
}

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
        if (idx >= sizeof(T) * 8)
        {
            throw std::out_of_range("Bit index exceeds type width");
        }

        return T(1) << idx;
    }

    template<typename T>
    using Scope = std::unique_ptr<T>;
    template<typename T, typename... Args>
    constexpr Scope<T> CreateScope(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }

} // namespace OloEngine

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
using uptr = uintptr_t;
using iptr = intptr_t;

// TODO(olbu): Consider adding using b8 = bool; ?

static const u64 u64_max = UINT64_MAX;
static const i64 i64_max = INT64_MAX;
static const u32 u32_max = UINT32_MAX;
static const i32 i32_max = INT32_MAX;
static const u16 u16_max = UINT16_MAX;
static const i16 i16_max = INT16_MAX;
static const u8 u8_max = UINT8_MAX;
static const i8 i8_max = INT8_MAX;

// Maximum values (UE-style naming)
static constexpr i32 MAX_i32 = i32_max;
static constexpr i64 MAX_i64 = i64_max;
static constexpr u32 MAX_u32 = u32_max;
static constexpr u64 MAX_u64 = u64_max;

// Index constants
static constexpr i32 INDEX_NONE = -1;

// Initialization enums (UE-style) ///////////////////////////////////////

/**
 * @enum EForceInit
 * @brief Used to explicitly request default initialization
 */
enum EForceInit
{
    ForceInit,
    ForceInitToZero
};

/**
 * @enum ENoInit
 * @brief Used to skip initialization for performance
 */
enum ENoInit
{
    NoInit
};

/**
 * @enum EConstEval
 * @brief Used to add an explicitly consteval constructor when the default constructor
 *        cannot be made constexpr (e.g., avoiding zero-initialization at runtime)
 */
enum EConstEval
{
    ConstEval
};

/**
 * @enum EInPlace
 * @brief Used to construct in-place
 */
enum EInPlace
{
    InPlace
};

//==============================================================================
/// Flag utilities for state tracking
namespace OloEngine
{
    /**
     * Thread-safe atomic flag for inter-thread communication
     * Based on Hazel's AtomicFlag implementation
     */
    struct AtomicFlag
    {
        OLO_FINLINE void SetDirty() noexcept
        {
            m_Flag.store(true, std::memory_order_release);
        }
        OLO_FINLINE bool CheckAndResetIfDirty() noexcept
        {
            return m_Flag.exchange(false, std::memory_order_acq_rel);
        }

        // Initialize to "not dirty" state (false)
        AtomicFlag() noexcept : m_Flag(false) {}
        AtomicFlag(const AtomicFlag&) = delete;
        AtomicFlag& operator=(const AtomicFlag&) = delete;
        AtomicFlag(AtomicFlag&&) = delete;
        AtomicFlag& operator=(AtomicFlag&&) = delete;

      private:
        std::atomic<bool> m_Flag;
    };

    /**
     * Simple flag for tracking dirty state (single-threaded)
     * Based on Hazel's Flag implementation
     */
    struct Flag
    {
        OLO_FINLINE void SetDirty() noexcept
        {
            m_Flag = true;
        }
        OLO_FINLINE bool CheckAndResetIfDirty() noexcept
        {
            return std::exchange(m_Flag, false);
        }

        OLO_FINLINE bool IsDirty() const noexcept
        {
            return m_Flag;
        }

        // Explicitly delete copy and move operations to avoid accidental copying of dirty state
        Flag() = default;
        Flag(const Flag&) = delete;
        Flag& operator=(const Flag&) = delete;
        Flag(Flag&&) = delete;
        Flag& operator=(Flag&&) = delete;

      private:
        bool m_Flag = false;
    };

    /**
     * @struct FMath
     * @brief Basic math utilities (ported from UE)
     */
    struct FMath
    {
        /**
         * @brief Checks if a value is a power of two
         * @tparam T Integer type
         * @param Value The value to check
         * @return true if Value is a power of two, false otherwise (including for 0)
         */
        template<typename T>
        [[nodiscard]] static constexpr OLO_FINLINE bool IsPowerOfTwo(T Value)
        {
            return ((Value & (Value - 1)) == static_cast<T>(0));
        }

        /**
         * @brief Returns the minimum of two values
         */
        template<typename T>
        [[nodiscard]] static constexpr OLO_FINLINE T Min(T A, T B)
        {
            return (A <= B) ? A : B;
        }

        /**
         * @brief Returns the maximum of two values
         */
        template<typename T>
        [[nodiscard]] static constexpr OLO_FINLINE T Max(T A, T B)
        {
            return (A >= B) ? A : B;
        }
    };
} // namespace OloEngine

#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Assert.h"
