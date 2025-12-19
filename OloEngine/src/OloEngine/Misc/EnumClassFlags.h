// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include <type_traits>

// @file EnumClassFlags.h
// @brief Utilities for using enum classes as bit flags
//
// Provides macros and template functions to enable bitwise operations
// on enum class types, making them usable as flags.
//
// Ported from Unreal Engine's Misc/EnumClassFlags.h

// @def ENUM_CLASS_FLAGS
// @brief Defines all bitwise operators for enum classes so it can be (mostly) used as a regular flags enum
//
// Usage:
// @code
// enum class EMyFlags : u8
// {
//     None   = 0,
//     Flag1  = 1 << 0,
//     Flag2  = 1 << 1,
//     Flag3  = 1 << 2,
// };
// ENUM_CLASS_FLAGS(EMyFlags)
// @endcode
#define ENUM_CLASS_FLAGS(Enum)                                                                      \
    inline constexpr Enum& operator|=(Enum& Lhs, Enum Rhs)                                          \
    {                                                                                               \
        return Lhs = (Enum)(std::underlying_type_t<Enum>(Lhs) | std::underlying_type_t<Enum>(Rhs)); \
    }                                                                                               \
    inline constexpr Enum& operator&=(Enum& Lhs, Enum Rhs)                                          \
    {                                                                                               \
        return Lhs = (Enum)(std::underlying_type_t<Enum>(Lhs) & std::underlying_type_t<Enum>(Rhs)); \
    }                                                                                               \
    inline constexpr Enum& operator^=(Enum& Lhs, Enum Rhs)                                          \
    {                                                                                               \
        return Lhs = (Enum)(std::underlying_type_t<Enum>(Lhs) ^ std::underlying_type_t<Enum>(Rhs)); \
    }                                                                                               \
    inline constexpr Enum operator|(Enum Lhs, Enum Rhs)                                             \
    {                                                                                               \
        return (Enum)(std::underlying_type_t<Enum>(Lhs) | std::underlying_type_t<Enum>(Rhs));       \
    }                                                                                               \
    inline constexpr Enum operator&(Enum Lhs, Enum Rhs)                                             \
    {                                                                                               \
        return (Enum)(std::underlying_type_t<Enum>(Lhs) & std::underlying_type_t<Enum>(Rhs));       \
    }                                                                                               \
    inline constexpr Enum operator^(Enum Lhs, Enum Rhs)                                             \
    {                                                                                               \
        return (Enum)(std::underlying_type_t<Enum>(Lhs) ^ std::underlying_type_t<Enum>(Rhs));       \
    }                                                                                               \
    inline constexpr bool operator!(Enum E)                                                         \
    {                                                                                               \
        return !std::underlying_type_t<Enum>(E);                                                    \
    }                                                                                               \
    inline constexpr Enum operator~(Enum E)                                                         \
    {                                                                                               \
        return (Enum)~std::underlying_type_t<Enum>(E);                                              \
    }

// @def FRIEND_ENUM_CLASS_FLAGS
// @brief Friends all bitwise operators for enum classes so the definition can be kept private/protected
#define FRIEND_ENUM_CLASS_FLAGS(Enum)                       \
    friend constexpr Enum& operator|=(Enum& Lhs, Enum Rhs); \
    friend constexpr Enum& operator&=(Enum& Lhs, Enum Rhs); \
    friend constexpr Enum& operator^=(Enum& Lhs, Enum Rhs); \
    friend constexpr Enum operator|(Enum Lhs, Enum Rhs);    \
    friend constexpr Enum operator&(Enum Lhs, Enum Rhs);    \
    friend constexpr Enum operator^(Enum Lhs, Enum Rhs);    \
    friend constexpr bool operator!(Enum E);                \
    friend constexpr Enum operator~(Enum E);

// @brief Check if the Flags value contains all the flags specified by Contains
// @tparam Enum The enum type
// @param Flags The value to check
// @param Contains The flags that must all be present
// @return true if Flags contains all flags in Contains
template<typename Enum>
constexpr bool EnumHasAllFlags(Enum Flags, Enum Contains)
{
    using UnderlyingType = std::underlying_type_t<Enum>;
    return ((UnderlyingType)Flags & (UnderlyingType)Contains) == (UnderlyingType)Contains;
}

// @brief Check if the Flags value contains any of the flags specified by Contains
// @tparam Enum The enum type
// @param Flags The value to check
// @param Contains The flags to check for
// @return true if Flags contains at least one flag in Contains
template<typename Enum>
constexpr bool EnumHasAnyFlags(Enum Flags, Enum Contains)
{
    using UnderlyingType = std::underlying_type_t<Enum>;
    return ((UnderlyingType)Flags & (UnderlyingType)Contains) != 0;
}

// @brief Check if the Flags value contains only the flags specified by Contains
//
// It is not required that any of the flags in Contains be set in Flags and Flags may be
// any combination of the flags specified by Contains (including 0). But if Flags
// contains any flag not in Contains, this function will return false.
// This function may also be understood as !EnumHasAnyFlags(Flags, ~Contains).
//
// @tparam Enum The enum type
// @param Flags The value to check
// @param Contains The flags to check the value against
// @return true if Flags consists of only the flags specified by Contains. false otherwise.
// @note Returns true if Flags is empty.
template<typename Enum>
constexpr bool EnumOnlyContainsFlags(Enum Flags, Enum Contains)
{
    return EnumHasAllFlags(Contains, Flags);
}

// @brief Check if Flags has one and only one flag set
//
// This can also be thought of as a check for a power-of-2 value.
//
// @tparam Enum The enum type
// @param Flags The value to check
// @return true if Flags has only one flag set. false otherwise.
template<typename Enum>
constexpr bool EnumHasOneFlag(Enum Flags)
{
    using UnderlyingType = std::underlying_type_t<Enum>;
    return ((UnderlyingType)Flags != 0) && (((UnderlyingType)Flags & ((UnderlyingType)Flags - 1)) == 0);
}

// @brief Check if Flags has one and only one of the flags specified in OneOfFlags set
//
// @tparam Enum The enum type
// @param Flags The value to check
// @param OneOfFlags The flags to check the value for
// @return true if Flags has one and only one of the flags in OneOfFlags set. false otherwise.
template<typename Enum>
constexpr bool EnumHasAnyOneFlag(Enum Flags, Enum OneOfFlags)
{
    using UnderlyingType = std::underlying_type_t<Enum>;
    return EnumHasOneFlag((Enum)((UnderlyingType)Flags & (UnderlyingType)OneOfFlags));
}

// @brief Add flags to an existing flags value
// @tparam Enum The enum type
// @param Flags The flags value to modify
// @param FlagsToAdd The flags to add
template<typename Enum>
constexpr void EnumAddFlags(Enum& Flags, Enum FlagsToAdd)
{
    using UnderlyingType = std::underlying_type_t<Enum>;
    Flags = (Enum)((UnderlyingType)Flags | (UnderlyingType)FlagsToAdd);
}

// @brief Remove flags from an existing flags value
// @tparam Enum The enum type
// @param Flags The flags value to modify
// @param FlagsToRemove The flags to remove
template<typename Enum>
constexpr void EnumRemoveFlags(Enum& Flags, Enum FlagsToRemove)
{
    using UnderlyingType = std::underlying_type_t<Enum>;
    Flags = (Enum)((UnderlyingType)Flags & ~(UnderlyingType)FlagsToRemove);
}

// @brief Get the lowest set flag in a flags value
// @tparam Enum The enum type
// @param Flags The flags value
// @return The lowest set flag, or 0 if no flags are set
template<typename Enum>
constexpr Enum EnumLowestSetFlag(Enum Flags)
{
    using UnderlyingType = std::underlying_type_t<Enum>;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4146) // unary minus operator applied to unsigned type, result still unsigned
#endif
    return Flags & (Enum)(-(UnderlyingType)Flags);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

// @brief Remove the lowest set flag from a flags value
// @tparam Enum The enum type
// @param Flags The flags value
// @return The flags value with the lowest set flag removed
template<typename Enum>
constexpr Enum EnumRemoveLowestSetFlag(Enum Flags)
{
    using UnderlyingType = std::underlying_type_t<Enum>;
    return (Enum)((UnderlyingType)Flags & ((UnderlyingType)Flags - 1));
}

// @brief Count the number of set flags in a flags value
// @tparam Enum The enum type
// @param Flags The flags value
// @return The number of set flags
template<typename Enum>
constexpr int EnumNumSetFlags(Enum Flags)
{
    using UnderlyingType = std::underlying_type_t<Enum>;

    int Result = 0;
    UnderlyingType Int = (UnderlyingType)Flags;
    while (Int != 0)
    {
        ++Result;
        Int &= Int - 1;
    }
    return Result;
}
