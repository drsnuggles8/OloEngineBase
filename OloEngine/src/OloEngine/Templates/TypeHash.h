#pragma once

// @file TypeHash.h
// @brief Type hashing utilities for containers
// 
// Provides:
// - GetTypeHash(): Unified hashing for all scalar types (int, float, enum, pointer)
// - HashCombine(): Combines two hash values in a non-commutative way
// - HashCombineFast(): Faster hash combination for in-process use only
// - PointerHash(): Platform-aware pointer hashing with MurmurFinalize
// 
// Ported from Unreal Engine's Templates/TypeHash.h

#include "OloEngine/Core/Base.h"
#include <type_traits>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace OloEngine
{
    // Forward declarations
    template <typename T>
    class TSharedRef;
    template <typename T>
    class TSharedPtr;

    // @brief Combines two hash values to get a third.
    // 
    // Note: This function is not commutative.
    // 
    // WARNING: Do not use this to hash contiguous memory; use the CRC functions instead.
    // WARNING: This method to combine hashes is backwards compatible but has poor entropy.
    //          New uses should use HashCombineFast.
    //
    // This function is a modified version of this algorithm:
    // https://www.boost.org/doc/libs/1_35_0/doc/html/boost/hash_combine_id241013.html
    // magic constant from CityHash
    [[nodiscard]] inline constexpr u32 HashCombine(u32 A, u32 C)
    {
        u32 B = 0x9e3779b9;
        A += B;

        A -= B; A -= C; A ^= (C >> 13);
        B -= C; B -= A; B ^= (A << 8);
        C -= A; C -= B; C ^= (B >> 13);
        A -= B; A -= C; A ^= (C >> 12);
        B -= C; B -= A; B ^= (A << 16);
        C -= A; C -= B; C ^= (B >> 5);
        A -= B; A -= C; A ^= (C >> 3);
        B -= C; B -= A; B ^= (A << 10);
        C -= A; C -= B; C ^= (B >> 15);

        return C;
    }

    // @brief Combines two hash values to get a third.
    // 
    // Note: This function is not commutative.
    // Note: This is faster than HashCombine and suitable for new code.
    // 
    // WARNING: This hash is not stable across processes or sessions, so should only be
    //          used for creating hashes that will be used within one process session.
    //          For a stable hash based on memory, see CityHash.
    [[nodiscard]] inline constexpr u32 HashCombineFast(u32 A, u32 B)
    {
        return A ^ (B + 0x9e3779b9 + (A << 6) + (A >> 2));
    }

    // @brief 64-bit version of HashCombineFast
    [[nodiscard]] inline constexpr u64 HashCombineFast(u64 A, u64 B)
    {
        return A ^ (B + 0x9e3779b97f4a7c15ULL + (A << 12) + (A >> 4));
    }

    // @brief MurmurHash3 32-bit finalizer
    [[nodiscard]] inline constexpr u32 MurmurFinalize32(u32 Hash)
    {
        Hash ^= Hash >> 16;
        Hash *= 0x85ebca6b;
        Hash ^= Hash >> 13;
        Hash *= 0xc2b2ae35;
        Hash ^= Hash >> 16;
        return Hash;
    }

    // @brief MurmurHash3 64-bit finalizer
    [[nodiscard]] inline constexpr u64 MurmurFinalize64(u64 Hash)
    {
        Hash ^= Hash >> 33;
        Hash *= 0xff51afd7ed558ccdULL;
        Hash ^= Hash >> 33;
        Hash *= 0xc4ceb9fe1a85ec53ULL;
        Hash ^= Hash >> 33;
        return Hash;
    }

    // @brief Generates a hash for a pointer with good bit distribution
    // 
    // Strips the always-zero lower bits from the pointer and applies
    // MurmurFinalize for better distribution.
    [[nodiscard]] inline u32 PointerHash(const void* Key, u32 C = 0)
    {
        // Commonly used alignment for heap allocations
        constexpr u32 AlignBits = 4;
        
#if OLO_PLATFORM_64BITS
        // Truncation is fine since we're just hashing
        const u64 PtrInt = reinterpret_cast<uintptr_t>(Key) >> AlignBits;
        // MurmurFinalize64 to get good distribution from the 64-bit pointer
        u32 Hash = static_cast<u32>(MurmurFinalize64(PtrInt));
#else
        const u32 PtrInt = static_cast<u32>(reinterpret_cast<uintptr_t>(Key)) >> AlignBits;
        u32 Hash = MurmurFinalize32(PtrInt);
#endif

        if (C != 0)
        {
            Hash = HashCombineFast(Hash, C);
        }
        return Hash;
    }

    // ============================================================================
    // GetTypeHash implementations for fundamental types
    // ============================================================================

    // @brief Primary template for GetTypeHash - handles scalar types uniformly
    // 
    // This template handles:
    // - Integral types (int, float, etc.)
    // - Floating point types (reinterpret as integer bits)
    // - Enum types (cast to underlying type)
    // - Pointer types (use PointerHash)
    template <typename T>
    [[nodiscard]] inline std::enable_if_t<std::is_enum_v<T>, u32>
    GetTypeHash(T Value)
    {
        return GetTypeHash(static_cast<std::underlying_type_t<T>>(Value));
    }

    // Integral types
    [[nodiscard]] inline constexpr u32 GetTypeHash(i8 Value)
    {
        return static_cast<u32>(Value);
    }

    [[nodiscard]] inline constexpr u32 GetTypeHash(u8 Value)
    {
        return Value;
    }

    [[nodiscard]] inline constexpr u32 GetTypeHash(i16 Value)
    {
        return static_cast<u32>(Value);
    }

    [[nodiscard]] inline constexpr u32 GetTypeHash(u16 Value)
    {
        return Value;
    }

    [[nodiscard]] inline constexpr u32 GetTypeHash(i32 Value)
    {
        return static_cast<u32>(Value);
    }

    [[nodiscard]] inline constexpr u32 GetTypeHash(u32 Value)
    {
        return Value;
    }

    [[nodiscard]] inline constexpr u32 GetTypeHash(i64 Value)
    {
        return static_cast<u32>(Value) + static_cast<u32>(Value >> 32) * 23;
    }

    [[nodiscard]] inline constexpr u32 GetTypeHash(u64 Value)
    {
        return static_cast<u32>(Value) + static_cast<u32>(Value >> 32) * 23;
    }

    // Floating point types - reinterpret as integer bits
    [[nodiscard]] inline u32 GetTypeHash(f32 Value)
    {
        // Handle -0.0f == 0.0f
        if (Value == 0.0f)
        {
            Value = 0.0f;
        }
        u32 IntValue;
        std::memcpy(&IntValue, &Value, sizeof(f32));
        return IntValue;
    }

    [[nodiscard]] inline u32 GetTypeHash(f64 Value)
    {
        // Handle -0.0 == 0.0
        if (Value == 0.0)
        {
            Value = 0.0;
        }
        u64 IntValue;
        std::memcpy(&IntValue, &Value, sizeof(f64));
        return GetTypeHash(IntValue);
    }

    // Pointer types
    template <typename T>
    [[nodiscard]] inline u32 GetTypeHash(T* Ptr)
    {
        return PointerHash(Ptr);
    }

    // C-string
    [[nodiscard]] inline u32 GetTypeHash(const char* S)
    {
        // Simple djb2 hash for C-strings
        u32 Hash = 5381;
        while (*S)
        {
            Hash = ((Hash << 5) + Hash) + static_cast<u8>(*S++);
        }
        return Hash;
    }

    // std::string
    [[nodiscard]] inline u32 GetTypeHash(const std::string& S)
    {
        return GetTypeHash(S.c_str());
    }

    // std::string_view
    [[nodiscard]] inline u32 GetTypeHash(std::string_view S)
    {
        u32 Hash = 5381;
        for (char C : S)
        {
            Hash = ((Hash << 5) + Hash) + static_cast<u8>(C);
        }
        return Hash;
    }

    // std::pair
    template <typename A, typename B>
    [[nodiscard]] inline u32 GetTypeHash(const std::pair<A, B>& Pair)
    {
        return HashCombineFast(GetTypeHash(Pair.first), GetTypeHash(Pair.second));
    }

    // Smart pointer support
    template <typename T>
    [[nodiscard]] inline u32 GetTypeHash(const TSharedRef<T>& Ptr)
    {
        return PointerHash(Ptr.Get());
    }

    template <typename T>
    [[nodiscard]] inline u32 GetTypeHash(const TSharedPtr<T>& Ptr)
    {
        return PointerHash(Ptr.Get());
    }

} // namespace OloEngine
