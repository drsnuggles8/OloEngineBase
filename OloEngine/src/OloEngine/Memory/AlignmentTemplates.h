#pragma once

// @file AlignmentTemplates.h
// @brief Memory alignment utility templates for OloEngine
// 
// Provides compile-time and runtime utilities for:
// - Aligning values up to a boundary
// - Aligning values down to a boundary
// - Checking alignment
// - Power-of-two utilities
// 
// Ported from Unreal Engine's AlignmentTemplates.h

#include "OloEngine/Core/Base.h"
#include <type_traits>

namespace OloEngine
{
    // ========================================================================
    // Power of Two Utilities
    // ========================================================================

    // @brief Check if a value is a power of two
    // @tparam T Integral type
    // @param Value The value to check
    // @return true if Value is a power of two, false otherwise
    template<typename T>
    constexpr bool IsPowerOfTwo(T Value)
    {
        static_assert(std::is_integral_v<T>, "IsPowerOfTwo requires an integral type");
        return Value > 0 && (Value & (Value - 1)) == 0;
    }

    // @brief Round up to the next power of two
    // @tparam T Integral type
    // @param Value The value to round up
    // @return The smallest power of two >= Value
    template<typename T>
    constexpr T RoundUpToPowerOfTwo(T Value)
    {
        static_assert(std::is_integral_v<T>, "RoundUpToPowerOfTwo requires an integral type");
        
        if (Value == 0)
            return 1;
        
        --Value;
        Value |= Value >> 1;
        Value |= Value >> 2;
        Value |= Value >> 4;
        if constexpr (sizeof(T) >= 2) Value |= Value >> 8;
        if constexpr (sizeof(T) >= 4) Value |= Value >> 16;
        if constexpr (sizeof(T) >= 8) Value |= Value >> 32;
        ++Value;
        
        return Value;
    }

    // @brief Compute the ceiling of log2 for an integer
    // @tparam T Integral type
    // @param Value The value to compute log2 of
    // @return Ceiling of log2(Value)
    template<typename T>
    constexpr u32 CeilLogTwo(T Value)
    {
        static_assert(std::is_integral_v<T>, "CeilLogTwo requires an integral type");
        
        if (Value <= 1)
            return 0;
        
        u32 Result = 0;
        --Value;
        while (Value > 0)
        {
            Value >>= 1;
            ++Result;
        }
        return Result;
    }

    // @brief Compute the floor of log2 for an integer
    // @tparam T Integral type
    // @param Value The value to compute log2 of
    // @return Floor of log2(Value)
    template<typename T>
    constexpr u32 FloorLogTwo(T Value)
    {
        static_assert(std::is_integral_v<T>, "FloorLogTwo requires an integral type");
        
        if (Value <= 0)
            return 0;
        
        u32 Result = 0;
        while (Value > 1)
        {
            Value >>= 1;
            ++Result;
        }
        return Result;
    }

    // ========================================================================
    // Alignment Utilities
    // ========================================================================

    // @brief Align a value to the nearest higher multiple of 'Alignment'
    // 
    // @tparam T The type of value to align (must be integral or pointer)
    // @param Val The value to align
    // @param Alignment The alignment boundary (must be a power of two)
    // @return The value aligned up to the specified alignment
    // 
    // @example
    //   Align(5, 4) == 8
    //   Align(8, 4) == 8
    //   Align(0, 4) == 0
    template<typename T>
    OLO_FINLINE constexpr T Align(T Val, u64 Alignment)
    {
        static_assert(std::is_integral_v<T> || std::is_pointer_v<T>, 
                     "Align expects an integer or pointer type");
        
        return static_cast<T>(((static_cast<u64>(Val) + Alignment - 1) & ~(Alignment - 1)));
    }

    // @brief Align a pointer to the nearest higher multiple of 'Alignment'
    // 
    // @tparam T The pointer type
    // @param Ptr The pointer to align
    // @param Alignment The alignment boundary (must be a power of two)
    // @return The pointer aligned up to the specified alignment
    template<typename T>
    OLO_FINLINE T* Align(T* Ptr, sizet Alignment)
    {
        return reinterpret_cast<T*>(
            (reinterpret_cast<sizet>(Ptr) + Alignment - 1) & ~(Alignment - 1)
        );
    }

    // @brief Align a value to the nearest lower multiple of 'Alignment'
    // 
    // @tparam T The type of value to align (must be integral or pointer)
    // @param Val The value to align
    // @param Alignment The alignment boundary (must be a power of two)
    // @return The value aligned down to the specified alignment
    // 
    // @example
    //   AlignDown(5, 4) == 4
    //   AlignDown(8, 4) == 8
    //   AlignDown(3, 4) == 0
    template<typename T>
    OLO_FINLINE constexpr T AlignDown(T Val, u64 Alignment)
    {
        static_assert(std::is_integral_v<T> || std::is_pointer_v<T>, 
                     "AlignDown expects an integer or pointer type");
        
        return static_cast<T>((static_cast<u64>(Val)) & ~(Alignment - 1));
    }

    // @brief Align a pointer to the nearest lower multiple of 'Alignment'
    // 
    // @tparam T The pointer type
    // @param Ptr The pointer to align
    // @param Alignment The alignment boundary (must be a power of two)
    // @return The pointer aligned down to the specified alignment
    template<typename T>
    OLO_FINLINE T* AlignDown(T* Ptr, sizet Alignment)
    {
        return reinterpret_cast<T*>(
            reinterpret_cast<sizet>(Ptr) & ~(Alignment - 1)
        );
    }

    // @brief Check if a value is aligned to the specified alignment
    // 
    // @tparam T The type of value to check (must be integral or pointer)
    // @param Val The value to check
    // @param Alignment The alignment boundary (must be a power of two)
    // @return true if the value is aligned, false otherwise
    template<typename T>
    OLO_FINLINE constexpr bool IsAligned(T Val, u64 Alignment)
    {
        static_assert(std::is_integral_v<T> || std::is_pointer_v<T>, 
                     "IsAligned expects an integer or pointer type");
        
        return !(static_cast<u64>(Val) & (Alignment - 1));
    }

    // @brief Check if a pointer is aligned to the specified alignment
    // 
    // @tparam T The pointer type
    // @param Ptr The pointer to check
    // @param Alignment The alignment boundary (must be a power of two)
    // @return true if the pointer is aligned, false otherwise
    template<typename T>
    OLO_FINLINE bool IsAligned(T* Ptr, sizet Alignment)
    {
        return !(reinterpret_cast<sizet>(Ptr) & (Alignment - 1));
    }

    // @brief Align a value to the nearest higher multiple of 'Alignment' (arbitrary, not necessarily power of two)
    // 
    // @tparam T The type of value to align (must be integral or pointer)
    // @param Val The value to align
    // @param Alignment The alignment boundary (can be any positive value)
    // @return The value aligned up to the specified alignment
    template<typename T>
    OLO_FINLINE constexpr T AlignArbitrary(T Val, u64 Alignment)
    {
        static_assert(std::is_integral_v<T> || std::is_pointer_v<T>, 
                     "AlignArbitrary expects an integer or pointer type");
        
        return static_cast<T>((((static_cast<u64>(Val) + Alignment - 1) / Alignment) * Alignment));
    }

} // namespace OloEngine
