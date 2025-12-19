#pragma once

// @file UnrealTypeTraits.h
// @brief Type trait utilities for OloEngine's memory and template systems
// 
// Provides type traits for:
// - Inheritance checking
// - Trivial type detection
// - Zero-constructability
// - Move/copy semantics detection
// 
// Ported from Unreal Engine's Templates/UnrealTypeTraits.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include <type_traits>
#include <utility>

// ============================================================================
// Macros to abstract the presence of certain compiler intrinsic type traits
// ============================================================================
#define HAS_TRIVIAL_CONSTRUCTOR(T) __has_trivial_constructor(T)
#define IS_POD(T) __is_pod(T)
#define IS_EMPTY(T) __is_empty(T)

namespace OloEngine
{
    // MoveTemp, MoveTempIfPossible, Forward, Swap, Exchange are in Templates/UnrealTemplate.h

    // ========================================================================
    // Inheritance Traits
    // ========================================================================

    // @struct TIsDerivedFrom
    // @brief Check if DerivedType is derived from BaseType
    // 
    // @tparam DerivedType The potentially derived type
    // @tparam BaseType The potential base type
    // 
    // @note This is similar to std::is_base_of but with a more intuitive name
    //       and matches UE's naming convention
    template<typename DerivedType, typename BaseType>
    struct TIsDerivedFrom
    {
        enum { Value = std::is_base_of_v<BaseType, DerivedType> };
        static constexpr bool value = std::is_base_of_v<BaseType, DerivedType>;
    };

    template<typename DerivedType, typename BaseType>
    inline constexpr bool TIsDerivedFrom_V = TIsDerivedFrom<DerivedType, BaseType>::value;

    // ========================================================================
    // Parameter Pack Utilities
    // ========================================================================

    // @struct TNthTypeFromParameterPack
    // @brief Gets the Nth type in a template parameter pack
    // 
    // @tparam N Index (0-based). N must be less than sizeof...(Types)
    // @tparam Types The parameter pack
    template <i32 N, typename... Types>
    struct TNthTypeFromParameterPack;

    template <i32 N, typename T, typename... OtherTypes>
    struct TNthTypeFromParameterPack<N, T, OtherTypes...>
    {
        using Type = typename TNthTypeFromParameterPack<N - 1, OtherTypes...>::Type;
    };

    template <typename T, typename... OtherTypes>
    struct TNthTypeFromParameterPack<0, T, OtherTypes...>
    {
        using Type = T;
    };

    template <i32 N, typename... Types>
    using TNthTypeFromParameterPack_T = typename TNthTypeFromParameterPack<N, Types...>::Type;

    // ========================================================================
    // Trivial Type Traits
    // ========================================================================

    // @struct TIsTriviallyCopyable
    // @brief Check if a type can be copied with memcpy
    template<typename T>
    struct TIsTriviallyCopyable
    {
        enum { Value = std::is_trivially_copyable_v<T> };
        static constexpr bool value = std::is_trivially_copyable_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsTriviallyCopyable_V = TIsTriviallyCopyable<T>::value;

    // @struct TIsTriviallyDestructible
    // @brief Check if a type has a trivial destructor (no cleanup needed)
    template<typename T>
    struct TIsTriviallyDestructible
    {
        enum { Value = std::is_trivially_destructible_v<T> };
        static constexpr bool value = std::is_trivially_destructible_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsTriviallyDestructible_V = TIsTriviallyDestructible<T>::value;

    // @struct TIsTriviallyConstructible
    // @brief Check if a type has a trivial default constructor
    template<typename T>
    struct TIsTriviallyConstructible
    {
        enum { Value = std::is_trivially_constructible_v<T> };
        static constexpr bool value = std::is_trivially_constructible_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsTriviallyConstructible_V = TIsTriviallyConstructible<T>::value;

    // @struct TIsTriviallyCopyConstructible
    // @brief Check if a type has a trivial copy constructor
    template<typename T>
    struct TIsTriviallyCopyConstructible
    {
        enum { Value = std::is_trivially_copy_constructible_v<T> };
        static constexpr bool value = std::is_trivially_copy_constructible_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsTriviallyCopyConstructible_V = TIsTriviallyCopyConstructible<T>::value;

    // @struct TIsTriviallyMoveConstructible
    // @brief Check if a type has a trivial move constructor
    template<typename T>
    struct TIsTriviallyMoveConstructible
    {
        enum { Value = std::is_trivially_move_constructible_v<T> };
        static constexpr bool value = std::is_trivially_move_constructible_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsTriviallyMoveConstructible_V = TIsTriviallyMoveConstructible<T>::value;

    // @struct TIsTriviallyMoveAssignable
    // @brief Check if a type has a trivial move assignment operator
    template<typename T>
    struct TIsTriviallyMoveAssignable
    {
        enum { Value = std::is_trivially_move_assignable_v<T> };
        static constexpr bool value = std::is_trivially_move_assignable_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsTriviallyMoveAssignable_V = TIsTriviallyMoveAssignable<T>::value;

    // @struct TIsTriviallyCopyAssignable
    // @brief Check if a type has a trivial copy assignment operator
    template<typename T>
    struct TIsTriviallyCopyAssignable
    {
        enum { Value = std::is_trivially_copy_assignable_v<T> };
        static constexpr bool value = std::is_trivially_copy_assignable_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsTriviallyCopyAssignable_V = TIsTriviallyCopyAssignable<T>::value;

    // ========================================================================
    // Zero Construction Traits
    // ========================================================================

    // @struct TIsZeroConstructType
    // @brief Check if a type can be "constructed" by simply zeroing its memory
    // 
    // This is true for types where zero-initialization produces a valid object.
    // Used to optimize bulk allocations by using memset(0) instead of calling constructors.
    // 
    // By default, this is true for arithmetic types and pointers.
    // Specialize this trait for your own types if zeroing produces a valid default state.
    template<typename T>
    struct TIsZeroConstructType
    {
        enum { Value = std::is_enum_v<T> || std::is_arithmetic_v<T> || std::is_pointer_v<T> };
        static constexpr bool value = Value;
    };

    template<typename T>
    inline constexpr bool TIsZeroConstructType_V = TIsZeroConstructType<T>::value;

    // Specializations for common types that are safe to zero-construct
    template<> struct TIsZeroConstructType<u8>  { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<u16> { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<u32> { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<u64> { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<i8>  { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<i16> { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<i32> { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<i64> { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<f32> { enum { Value = true }; static constexpr bool value = true; };
    template<> struct TIsZeroConstructType<f64> { enum { Value = true }; static constexpr bool value = true; };

    // ========================================================================
    // POD Traits
    // ========================================================================

    // @struct TIsPODType
    // @brief Check if a type is a POD (Plain Old Data) type
    // 
    // POD types can be:
    // - Copied with memcpy
    // - Zeroed with memset
    // - Don't need constructor/destructor calls
    template<typename T>
    struct TIsPODType
    {
        enum { Value = std::is_standard_layout_v<T> && std::is_trivial_v<T> };
        static constexpr bool value = Value;
    };

    template<typename T>
    inline constexpr bool TIsPODType_V = TIsPODType<T>::value;

    // ========================================================================
    // Format Specifier Traits
    // ========================================================================

    // @struct TFormatSpecifier
    // @brief Get printf-style format specifier for numeric types
    // 
    // The primary template will static_assert for unknown types.
    // Use Expose_TFormatSpecifier macro to add support for custom types.
    template<typename T>
    struct TFormatSpecifier
    {
        inline static constexpr const char* GetFormatSpecifier()
        {
            // Force the template instantiation to be dependent upon T
            static_assert(sizeof(T) < 0, "Format specifier not supported for this type.");
            return "Unknown";
        }
    };

    // @def Expose_TFormatSpecifier
    // @brief Macro to expose a format specifier for a type
    #define Expose_TFormatSpecifier(type, format) \
    template<> \
    struct TFormatSpecifier<type> \
    { \
        OLO_FINLINE static constexpr const char* GetFormatSpecifier() \
        { \
            return format; \
        } \
    };

    // Standard format specifiers for built-in types
    Expose_TFormatSpecifier(bool, "%i")
    Expose_TFormatSpecifier(u8, "%u")
    Expose_TFormatSpecifier(u16, "%u")
    Expose_TFormatSpecifier(u32, "%u")
    Expose_TFormatSpecifier(u64, "%llu")
    Expose_TFormatSpecifier(i8, "%d")
    Expose_TFormatSpecifier(i16, "%d")
    Expose_TFormatSpecifier(i32, "%d")
    Expose_TFormatSpecifier(i64, "%lld")
    Expose_TFormatSpecifier(f32, "%f")
    Expose_TFormatSpecifier(f64, "%f")
    Expose_TFormatSpecifier(long double, "%f")
    Expose_TFormatSpecifier(long, "%ld")
    Expose_TFormatSpecifier(unsigned long, "%lu")

    // ========================================================================
    // Type Name Traits
    // ========================================================================

    // @struct TNameOf
    // @brief Get the name of a type as a string
    // 
    // The primary template will assert for unknown types.
    // Use Expose_TNameOf macro to add support for custom types.
    template<typename T>
    struct TNameOf
    {
        inline static const char* GetName()
        {
            OLO_CORE_ASSERT(false, "TNameOf: Unknown type"); // request for the name of a type we do not know about
            return "Unknown";
        }
    };

    // @def Expose_TNameOf
    // @brief Macro to expose a type name
    #define Expose_TNameOf(type) \
    template<> \
    struct TNameOf<type> \
    { \
        OLO_FINLINE static const char* GetName() \
        { \
            return #type; \
        } \
    };

    // Standard type names for built-in types
    Expose_TNameOf(u8)
    Expose_TNameOf(u16)
    Expose_TNameOf(u32)
    Expose_TNameOf(u64)
    Expose_TNameOf(i8)
    Expose_TNameOf(i16)
    Expose_TNameOf(i32)
    Expose_TNameOf(i64)
    Expose_TNameOf(f32)
    Expose_TNameOf(f64)

    // ========================================================================
    // Reference/Pointer Manipulation
    // ========================================================================

    // @struct TRemoveReference
    // @brief Remove reference qualifiers from a type
    template<typename T>
    struct TRemoveReference
    {
        using Type = std::remove_reference_t<T>;
    };

    template<typename T>
    using TRemoveReference_T = typename TRemoveReference<T>::Type;

    // TRemovePointer is defined in Templates/UnrealTemplate.h

    // @struct TRemoveCV
    // @brief Remove const/volatile qualifiers from a type
    template<typename T>
    struct TRemoveCV
    {
        using Type = std::remove_cv_t<T>;
    };

    template<typename T>
    using TRemoveCV_T = typename TRemoveCV<T>::Type;

    // @struct TDecay
    // @brief Decay a type (remove references, remove cv, array-to-pointer, function-to-pointer)
    template<typename T>
    struct TDecay
    {
        using Type = std::decay_t<T>;
    };

    template<typename T>
    using TDecay_T = typename TDecay<T>::Type;

    // ========================================================================
    // Conditional Type Selection
    // ========================================================================

    // @struct TConditional
    // @brief Select between two types based on a condition
    template<bool Condition, typename TrueType, typename FalseType>
    struct TConditional
    {
        using Type = std::conditional_t<Condition, TrueType, FalseType>;
    };

    template<bool Condition, typename TrueType, typename FalseType>
    using TConditional_T = typename TConditional<Condition, TrueType, FalseType>::Type;

    // @struct TEnableIf
    // @brief SFINAE helper - type only valid if condition is true
    // 
    // Primary template for false case - no Type member (causes substitution failure)
    template<bool Condition, typename T = void>
    struct TEnableIf
    {};

    // Specialization for true case - provides Type member
    template<typename T>
    struct TEnableIf<true, T>
    {
        using Type = T;
    };

    template<bool Condition, typename T = void>
    using TEnableIf_T = typename TEnableIf<Condition, T>::Type;

    // ========================================================================
    // Type Identity
    // ========================================================================

    // @struct TTypeIdentity
    // @brief Returns the type unchanged - useful to prevent type deduction
    template<typename T>
    struct TTypeIdentity
    {
        using Type = T;
    };

    template<typename T>
    using TTypeIdentity_T = typename TTypeIdentity<T>::Type;

    // ========================================================================
    // Same Type Check
    // ========================================================================

    // @struct TIsSame
    // @brief Check if two types are the same
    template<typename A, typename B>
    struct TIsSame
    {
        enum { Value = std::is_same_v<A, B> };
        static constexpr bool value = std::is_same_v<A, B>;
    };

    template<typename A, typename B>
    inline constexpr bool TIsSame_V = TIsSame<A, B>::value;

    // ========================================================================
    // Allocator/Container Support Traits
    // ========================================================================

    // @struct TTypeCompatibleBytes
    // @brief Provides storage suitable for holding a type T, with proper alignment
    // 
    // Ported from UE5.7 Templates/TypeCompatibleBytes.h
    // 
    // Trivially constructible and destructible - users are responsible for 
    // managing the lifetime of the inner element.
    // 
    // Useful for placement new and type-erased storage.
    template<typename T>
    struct alignas(alignof(T)) TTypeCompatibleBytes
    {
        using ElementTypeAlias_NatVisHelper = T;

        // Trivially constructible and destructible
        TTypeCompatibleBytes() = default;
        ~TTypeCompatibleBytes() = default;

        // Non-copyable
        TTypeCompatibleBytes(TTypeCompatibleBytes&&) = delete;
        TTypeCompatibleBytes(const TTypeCompatibleBytes&) = delete;
        TTypeCompatibleBytes& operator=(TTypeCompatibleBytes&&) = delete;
        TTypeCompatibleBytes& operator=(const TTypeCompatibleBytes&) = delete;

        // Legacy accessor for backwards compatibility
        T* GetTypedPtr() { return reinterpret_cast<T*>(Pad); }
        const T* GetTypedPtr() const { return reinterpret_cast<const T*>(Pad); }

        using MutableGetType = T&;       // The type returned by Bytes.Get() where Bytes is a non-const lvalue
        using ConstGetType   = const T&; // The type returned by Bytes.Get() where Bytes is a const lvalue
        using RvalueGetType  = T&&;      // The type returned by Bytes.Get() where Bytes is an rvalue (non-const)

        // Gets the inner element - no checks are performed to ensure an element is present.
        T& GetUnchecked() & { return *reinterpret_cast<T*>(Pad); }
        const T& GetUnchecked() const& { return *reinterpret_cast<const T*>(Pad); }
        T&& GetUnchecked() && { return static_cast<T&&>(*reinterpret_cast<T*>(Pad)); }

        // Emplaces an inner element.
        // Note: no checks are possible to ensure that an element isn't already present.
        // DestroyUnchecked() must be called to end the element's lifetime.
        template <typename... ArgTypes>
        void EmplaceUnchecked(ArgTypes&&... Args)
        {
            new (static_cast<void*>(Pad)) T(Forward<ArgTypes>(Args)...);
        }

        // Destroys the inner element.
        // Note: no checks are possible to ensure that there is an element already present.
        void DestroyUnchecked()
        {
            reinterpret_cast<T*>(Pad)->~T();
        }

        u8 Pad[sizeof(T)];
    };

    // Specialization for void
    template<>
    struct TTypeCompatibleBytes<void>
    {
        using ElementTypeAlias_NatVisHelper = void;

        TTypeCompatibleBytes() = default;
        ~TTypeCompatibleBytes() = default;

        TTypeCompatibleBytes(TTypeCompatibleBytes&&) = delete;
        TTypeCompatibleBytes(const TTypeCompatibleBytes&) = delete;
        TTypeCompatibleBytes& operator=(TTypeCompatibleBytes&&) = delete;
        TTypeCompatibleBytes& operator=(const TTypeCompatibleBytes&) = delete;

        using MutableGetType = void;
        using ConstGetType   = void;
        using RvalueGetType  = void;

        void GetUnchecked() const {}
        void EmplaceUnchecked() {}
        void DestroyUnchecked() {}
    };

    // @struct TAlignedBytes
    // @brief Provides storage of specified size and alignment
    // 
    // @tparam Size Size in bytes
    // @tparam Alignment Alignment requirement
    template<sizet Size, sizet Alignment>
    struct alignas(Alignment) TAlignedBytes
    {
        u8 Bytes[Size];
    };

    // @struct TChooseClass
    // @brief Select between two classes based on a condition (similar to TConditional)
    template<bool Predicate, typename TrueClass, typename FalseClass>
    struct TChooseClass
    {
        using Result = TrueClass;
    };

    template<typename TrueClass, typename FalseClass>
    struct TChooseClass<false, TrueClass, FalseClass>
    {
        using Result = FalseClass;
    };

    template<bool Predicate, typename TrueClass, typename FalseClass>
    using TChooseClass_T = typename TChooseClass<Predicate, TrueClass, FalseClass>::Result;

    // ========================================================================
    // Void Type for SFINAE
    // ========================================================================

    // @struct TVoid
    // @brief Helper for creating void_t pattern in pre-C++17 style
    template<typename...>
    using TVoid = void;

    // ========================================================================
    // Has Member Detection (Concept-like before C++20)
    // ========================================================================

    namespace Detail
    {
        template<typename T, typename = void>
        struct THasGetTypeHash : std::false_type {};

        template<typename T>
        struct THasGetTypeHash<T, TVoid<decltype(GetTypeHash(std::declval<T>()))>> : std::true_type {};
    }

    // @struct TIsTriviallyRelocatable
    // @brief Trait to mark types as trivially relocatable (memcpy'able for moves)
    // 
    // Types that are trivially relocatable can have their instances moved in memory
    // by using memcpy instead of calling their move constructor and destructor.
    // This is a significant optimization for containers.
    // 
    // By default, types are assumed to be trivially relocatable. If your type
    // contains pointers/references to itself (e.g., std::list nodes), you should
    // specialize this to false.
    // 
    // @note UE assumes all types are trivially relocatable by default for performance.
    template <typename T>
    struct TIsTriviallyRelocatable
    {
        enum { Value = true };
    };

    // Handle cv-qualifiers
    template <typename T> struct TIsTriviallyRelocatable<const T>          { enum { Value = TIsTriviallyRelocatable<T>::Value }; };
    template <typename T> struct TIsTriviallyRelocatable<volatile T>       { enum { Value = TIsTriviallyRelocatable<T>::Value }; };
    template <typename T> struct TIsTriviallyRelocatable<const volatile T> { enum { Value = TIsTriviallyRelocatable<T>::Value }; };

    template<typename T>
    inline constexpr bool TIsTriviallyRelocatable_V = TIsTriviallyRelocatable<T>::Value;

    // @struct TUseBitwiseSwap
    // @brief Determines if bitwise operations (memcpy/memswap) should be used for relocation
    // 
    // For small 'register' types (pointers, integers, floats, enums), using memcpy
    // forces them into memory and is slower than direct register moves.
    // For larger types (structs, classes), memcpy is more efficient.
    // 
    // This is checked in RelocateConstructItem for single-element relocations.
    // Bulk operations always use memcpy due to loop overhead.
    template <typename T>
    struct TUseBitwiseSwap
    {
        // Don't use bitwise operations for 'register' types - forces them into memory and is slower
        enum { Value = !(std::is_enum_v<T> || std::is_pointer_v<T> || std::is_arithmetic_v<T>) };
    };

    template<typename T>
    inline constexpr bool TUseBitwiseSwap_V = TUseBitwiseSwap<T>::Value;

    // @struct TCanBitwiseRelocate
    // @brief Check if a type can be relocated using memcpy (move + destroy in one step)
    // 
    // Types that can be bitwise relocated:
    // - Are trivially copyable and trivially destructible, OR
    // - Have been explicitly marked as relocatable
     */
    template<typename T>
    struct TCanBitwiseRelocate
    {
        enum
        {
            Value = TIsTriviallyCopyable<T>::Value && TIsTriviallyDestructible<T>::Value
        };
        static constexpr bool value = Value;
    };

    template<typename T>
    inline constexpr bool TCanBitwiseRelocate_V = TCanBitwiseRelocate<T>::value;

    // ========================================================================
    // Reference Type Traits (needed by TIsBitwiseConstructible)
    // ========================================================================

    // @struct TIsReferenceType
    // @brief Check if a type is a reference (lvalue or rvalue)
    template<typename T> struct TIsReferenceType      { enum { Value = false }; };
    template<typename T> struct TIsReferenceType<T&>  { enum { Value = true  }; };
    template<typename T> struct TIsReferenceType<T&&> { enum { Value = true  }; };

    template<typename T>
    inline constexpr bool TIsReferenceType_V = TIsReferenceType<T>::Value;

    // @struct TIsFundamentalType
    // @brief Check if a type is a fundamental type (arithmetic or void)
    template<typename T>
    struct TIsFundamentalType
    {
        enum { Value = std::is_arithmetic_v<T> || std::is_void_v<T> };
        static constexpr bool value = Value;
    };

    template<typename T>
    inline constexpr bool TIsFundamentalType_V = TIsFundamentalType<T>::Value;

    // @struct TIsFunction
    // @brief Check if a type is a function type
    template <typename T>
    struct TIsFunction
    {
        enum { Value = false };
    };

    template <typename RetType, typename... Params>
    struct TIsFunction<RetType(Params...)>
    {
        enum { Value = true };
    };

    template<typename T>
    inline constexpr bool TIsFunction_V = TIsFunction<T>::Value;

    // @struct TIsLValueReferenceType
    // @brief Check if a type is an lvalue reference
    template<typename T> struct TIsLValueReferenceType     { enum { Value = false }; };
    template<typename T> struct TIsLValueReferenceType<T&> { enum { Value = true  }; };

    template<typename T>
    inline constexpr bool TIsLValueReferenceType_V = TIsLValueReferenceType<T>::Value;

    // @struct TIsRValueReferenceType
    // @brief Check if a type is an rvalue reference
    template<typename T> struct TIsRValueReferenceType      { enum { Value = false }; };
    template<typename T> struct TIsRValueReferenceType<T&&> { enum { Value = true  }; };

    template<typename T>
    inline constexpr bool TIsRValueReferenceType_V = TIsRValueReferenceType<T>::Value;

    // @struct TIsArithmetic
    // @brief Check if a type is an arithmetic type (integral or floating point)
    template<typename T>
    struct TIsArithmetic
    {
        enum { Value = std::is_arithmetic_v<T> };
        static constexpr bool value = std::is_arithmetic_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsArithmetic_V = TIsArithmetic<T>::Value;

    // @struct TIsEnum
    // @brief Check if a type is an enumeration
    template<typename T>
    struct TIsEnum
    {
        enum { Value = std::is_enum_v<T> };
        static constexpr bool value = std::is_enum_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsEnum_V = TIsEnum<T>::Value;

    // @struct TIsPointer
    // @brief Check if a type is a pointer
    template<typename T>
    struct TIsPointer
    {
        enum { Value = std::is_pointer_v<T> };
        static constexpr bool value = std::is_pointer_v<T>;
    };

    template<typename T>
    inline constexpr bool TIsPointer_V = TIsPointer<T>::Value;

    // ========================================================================
    // Logical Combinators (needed by TCallTraits)
    // ========================================================================

    // @struct TAnd
    // @brief Logical AND of type traits
    template<typename... Args>
    struct TAnd;

    template<>
    struct TAnd<>
    {
        enum { Value = true };
    };

    template<typename First, typename... Rest>
    struct TAnd<First, Rest...>
    {
        enum { Value = First::Value && TAnd<Rest...>::Value };
    };

    // @struct TOr
    // @brief Logical OR of type traits
    template<typename... Args>
    struct TOr;

    template<>
    struct TOr<>
    {
        enum { Value = false };
    };

    template<typename First, typename... Rest>
    struct TOr<First, Rest...>
    {
        enum { Value = First::Value || TOr<Rest...>::Value };
    };

    // @struct TNot
    // @brief Logical NOT of a type trait
    template<typename T>
    struct TNot
    {
        enum { Value = !T::Value };
    };

    // @struct TAndValue
    // @brief Helper to combine a boolean value with type traits in TAnd/TOr
    template<bool Val, typename... Args>
    struct TAndValue
    {
        enum { Value = Val && TAnd<Args...>::Value };
    };

    // ========================================================================
    // Call Traits (UE-specific optimization for parameter passing)
    // ========================================================================

    // @struct TCallTraitsParamTypeHelper
    // @brief Helper for determining optimal parameter type
    template <typename T, bool TypeIsSmall>
    struct TCallTraitsParamTypeHelper
    {
        using ParamType = const T&;
        using ConstParamType = const T&;
    };

    template <typename T>
    struct TCallTraitsParamTypeHelper<T, true>
    {
        using ParamType = const T;
        using ConstParamType = const T;
    };

    template <typename T>
    struct TCallTraitsParamTypeHelper<T*, true>
    {
        using ParamType = T*;
        using ConstParamType = const T*;
    };

    // @struct TCallTraitsBase
    // @brief Base class for call traits
    // 
    // Determines the optimal way to pass a type as a function parameter.
    // Small POD types are passed by value, larger types by const reference.
    template <typename T>
    struct TCallTraitsBase
    {
    private:
        enum { PassByValue = TOr<TAndValue<(sizeof(T) <= sizeof(void*)), TIsPODType<T>>, TIsArithmetic<T>>::Value };

    public:
        using ValueType = T;
        using Reference = T&;
        using ConstReference = const T&;
        using ParamType = typename TCallTraitsParamTypeHelper<T, PassByValue>::ParamType;
        using ConstPointerType = typename TCallTraitsParamTypeHelper<T, PassByValue>::ConstParamType;
    };

    // @struct TCallTraits
    // @brief Determines optimal parameter passing conventions for a type
    // 
    // The main member to note is ParamType, which specifies the optimal
    // form to pass the type as a parameter to a function.
    // 
    // Has a small-value optimization when a type is POD and as small as a pointer.
    template <typename T>
    struct TCallTraits : public TCallTraitsBase<T> {};

    // Fix reference-to-reference problems
    template <typename T>
    struct TCallTraits<T&>
    {
        using ValueType = T&;
        using Reference = T&;
        using ConstReference = const T&;
        using ParamType = T&;
        using ConstPointerType = T&;
    };

    // Array types
    template <typename T, sizet N>
    struct TCallTraits<T[N]>
    {
    private:
        using ArrayType = T[N];
    public:
        using ValueType = const T*;
        using Reference = ArrayType&;
        using ConstReference = const ArrayType&;
        using ParamType = const T* const;
        using ConstPointerType = const T* const;
    };

    // Const array types
    template <typename T, sizet N>
    struct TCallTraits<const T[N]>
    {
    private:
        using ArrayType = const T[N];
    public:
        using ValueType = const T*;
        using Reference = ArrayType&;
        using ConstReference = const ArrayType&;
        using ParamType = const T* const;
        using ConstPointerType = const T* const;
    };

    // ========================================================================
    // Type Traits for Containers (UE-specific)
    // ========================================================================

    // @struct TTypeTraitsBase
    // @brief Base traits for container element types
    // 
    // Provides:
    // - ConstInitType: Optimal type for initialization parameters
    // - IsBytewiseComparable: Whether memcmp can be used for comparison
    template<typename T>
    struct TTypeTraitsBase
    {
        using ConstInitType = typename TCallTraits<T>::ParamType;
        using ConstPointerType = typename TCallTraits<T>::ConstPointerType;

        // Assume bytewise comparable for enums, arithmetic types, and pointers
        // Users should specialize for custom types
        enum { IsBytewiseComparable = TOr<TIsEnum<T>, TIsArithmetic<T>, TIsPointer<T>>::Value };
    };

    // @struct TTypeTraits
    // @brief Traits for types used in containers
    template<typename T>
    struct TTypeTraits : public TTypeTraitsBase<T> {};

    // ========================================================================
    // Move Support Traits (UE-specific optimization)
    // ========================================================================

    // @struct TMoveSupportTraitsBase
    // @brief Base for move support traits
    template <typename T, typename U>
    struct TMoveSupportTraitsBase
    {
        // Param type is not a const lvalue reference, which means it's pass-by-value,
        // so we should just provide a single type for copying.
        // Move overloads will be ignored due to SFINAE.
        using Copy = U;
    };

    template <typename T>
    struct TMoveSupportTraitsBase<T, const T&>
    {
        // Param type is a const lvalue reference, so we can provide an overload for moving.
        using Copy = const T&;
        using Move = T&&;
    };

    // @struct TMoveSupportTraits
    // @brief Traits for efficient move-aware function overloads
    // 
    // Usage:
    // @code
    // template <typename T>
    // void Func(typename TMoveSupportTraits<T>::Copy Obj) { // Copy Obj }
    // 
    // template <typename T>
    // void Func(typename TMoveSupportTraits<T>::Move Obj) { // Move from Obj }
    // @endcode
    // 
    // This handles pass-by-value types (ints, floats) which should never have a reference overload.
    template <typename T>
    struct TMoveSupportTraits : TMoveSupportTraitsBase<T, typename TCallTraits<T>::ParamType>
    {
    };

    // ========================================================================
    // Bitwise Constructible (UE-specific container optimization)
    // ========================================================================

    // @struct TIsBitwiseConstructible
    // @brief Tests if type T can be constructed from type U using memcpy
    // 
    // This is used by containers to optimize construction when possible.
    // 
    // Examples:
    // - TIsBitwiseConstructible<PODType, PODType>::Value == true  (PODs can be trivially copied)
    // - TIsBitwiseConstructible<const int*, int*>::Value == true  (non-const to const pointer)
    // - TIsBitwiseConstructible<int*, const int*>::Value == false (const-correctness violation)
    // - TIsBitwiseConstructible<i32, u32>::Value == true  (signed/unsigned conversion)
    template <typename T, typename Arg>
    struct TIsBitwiseConstructible
    {
        static_assert(
            !TIsReferenceType<T>::Value &&
            !TIsReferenceType<Arg>::Value,
            "TIsBitwiseConstructible is not designed to accept reference types");

        static_assert(
            std::is_same_v<T, std::remove_cv_t<T>> &&
            std::is_same_v<Arg, std::remove_cv_t<Arg>>,
            "TIsBitwiseConstructible is not designed to accept qualified types");

        // Assume no bitwise construction in general
        enum { Value = false };
    };

    // T can always be bitwise constructed from itself if trivially copy constructible
    template <typename T>
    struct TIsBitwiseConstructible<T, T>
    {
        enum { Value = TIsTriviallyCopyConstructible<T>::Value };
    };

    // Constructing a const T is the same as constructing a T
    template <typename T, typename U>
    struct TIsBitwiseConstructible<const T, U> : TIsBitwiseConstructible<T, U>
    {
    };

    // Const pointers can be bitwise constructed from non-const pointers
    template <typename T>
    struct TIsBitwiseConstructible<const T*, T*>
    {
        enum { Value = true };
    };

    // Signed/unsigned integer conversions (two's complement)
    template <> struct TIsBitwiseConstructible<u8,  i8>  { enum { Value = true }; };
    template <> struct TIsBitwiseConstructible<i8,  u8>  { enum { Value = true }; };
    template <> struct TIsBitwiseConstructible<u16, i16> { enum { Value = true }; };
    template <> struct TIsBitwiseConstructible<i16, u16> { enum { Value = true }; };
    template <> struct TIsBitwiseConstructible<u32, i32> { enum { Value = true }; };
    template <> struct TIsBitwiseConstructible<i32, u32> { enum { Value = true }; };
    template <> struct TIsBitwiseConstructible<u64, i64> { enum { Value = true }; };
    template <> struct TIsBitwiseConstructible<i64, u64> { enum { Value = true }; };

    // char conversions (ANSICHAR equivalent)
    template <> struct TIsBitwiseConstructible<i8,  char> { enum { Value = true }; };
    template <> struct TIsBitwiseConstructible<u8,  char> { enum { Value = true }; };

    template <typename T, typename Arg>
    inline constexpr bool TIsBitwiseConstructible_V = TIsBitwiseConstructible<T, Arg>::Value;

    // ========================================================================
    // Bulk Serialization Trait
    // ========================================================================

    // @struct TCanBulkSerialize
    // @brief Trait indicating whether a type can be serialized in bulk (via memcpy)
    // 
    // Types that can be bulk serialized are trivially copyable types where the
    // binary representation is stable across platforms and versions.
    // 
    // Default is false for safety. Specialize for types known to be safe for bulk serialization.
    // 
    // Example of types safe for bulk serialization:
    // - Primitive numeric types with known endianness handling
    // - POD structs with no padding or pointer members
    // 
    // Example of types NOT safe for bulk serialization:
    // - Types with pointers or references
    // - Types with virtual functions
    // - Types with platform-dependent sizes
    // 
    // @tparam T The type to check
    template <typename T>
    struct TCanBulkSerialize
    {
        enum { Value = false };
    };

    template <typename T>
    inline constexpr bool TCanBulkSerialize_V = TCanBulkSerialize<T>::Value;

    // Specializations for types safe to bulk serialize
    // Note: Floating point formats are IEEE 754 on all supported platforms
    template <> struct TCanBulkSerialize<u8>  { enum { Value = true }; };
    template <> struct TCanBulkSerialize<i8>  { enum { Value = true }; };
    template <> struct TCanBulkSerialize<char> { enum { Value = true }; };
    // Larger types need endianness consideration during serialization

    // ========================================================================
    // Weak Pointer Type Trait
    // ========================================================================

    // @struct TIsWeakPointerType
    // @brief Check if a type is a weak pointer type
    // 
    // Specialize this for your weak pointer types.
    template<typename T>
    struct TIsWeakPointerType
    {
        enum { Value = false };
    };

    template<typename T>
    inline constexpr bool TIsWeakPointerType_V = TIsWeakPointerType<T>::Value;

    // ========================================================================
    // Virtual Destructor Helper
    // ========================================================================

    // @struct FVirtualDestructor
    // @brief Base class that provides a virtual destructor
    struct FVirtualDestructor
    {
        virtual ~FVirtualDestructor() = default;
    };

    // ========================================================================
    // Member Function Detection Macro
    // ========================================================================

    // @def GENERATE_MEMBER_FUNCTION_CHECK
    // @brief Generates a trait class to detect if a type has a specific member function
    // 
    // Usage:
    // @code
    // GENERATE_MEMBER_FUNCTION_CHECK(Serialize, void, const, FArchive&)
    // // Creates THasMemberFunction_Serialize<T> which has ::Value = true/false
    // @endcode
    #define GENERATE_MEMBER_FUNCTION_CHECK(MemberName, Result, ConstModifier, ...)                              \
    template <typename T>                                                                                       \
    class THasMemberFunction_##MemberName                                                                       \
    {                                                                                                           \
        template <typename U, Result(U::*)(__VA_ARGS__) ConstModifier> struct Check;                            \
        template <typename U> static char MemberTest(Check<U, &U::MemberName>*);                                \
        template <typename U> static int MemberTest(...);                                                       \
    public:                                                                                                     \
        enum { Value = sizeof(MemberTest<T>(nullptr)) == sizeof(char) };                                        \
    };

    // ========================================================================
    // Container Type Traits
    // ========================================================================

    // @struct TIsContiguousContainer
    // @brief Type trait to detect if a container provides contiguous storage
    // 
    // A contiguous container stores elements in contiguous memory (like arrays).
    // Specialize this for container types that provide GetData() and GetNum().
    // 
    // Default is false - specialize for types that are contiguous containers.
    template <typename T>
    struct TIsContiguousContainer
    {
        static constexpr bool Value = false;
    };

    // cv-qualifier propagation
    template <typename T> struct TIsContiguousContainer<             T& > : TIsContiguousContainer<T> {};
    template <typename T> struct TIsContiguousContainer<             T&&> : TIsContiguousContainer<T> {};
    template <typename T> struct TIsContiguousContainer<const          T> : TIsContiguousContainer<T> {};
    template <typename T> struct TIsContiguousContainer<      volatile T> : TIsContiguousContainer<T> {};
    template <typename T> struct TIsContiguousContainer<const volatile T> : TIsContiguousContainer<T> {};

    // C-array specializations (always contiguous)
    template <typename T, sizet N> struct TIsContiguousContainer<               T[N]> { static constexpr bool Value = true; };
    template <typename T, sizet N> struct TIsContiguousContainer<const          T[N]> { static constexpr bool Value = true; };
    template <typename T, sizet N> struct TIsContiguousContainer<      volatile T[N]> { static constexpr bool Value = true; };
    template <typename T, sizet N> struct TIsContiguousContainer<const volatile T[N]> { static constexpr bool Value = true; };

    // Zero-sized array specializations
#ifndef __clang__
    template <typename T> struct TIsContiguousContainer<               T[0]> { static constexpr bool Value = true; };
    template <typename T> struct TIsContiguousContainer<const          T[0]> { static constexpr bool Value = true; };
    template <typename T> struct TIsContiguousContainer<      volatile T[0]> { static constexpr bool Value = true; };
    template <typename T> struct TIsContiguousContainer<const volatile T[0]> { static constexpr bool Value = true; };
#endif

    // Unbounded C-arrays (never contiguous - should be treated as pointers)
    template <typename T> struct TIsContiguousContainer<               T[]> { static constexpr bool Value = false; };
    template <typename T> struct TIsContiguousContainer<const          T[]> { static constexpr bool Value = false; };
    template <typename T> struct TIsContiguousContainer<      volatile T[]> { static constexpr bool Value = false; };
    template <typename T> struct TIsContiguousContainer<const volatile T[]> { static constexpr bool Value = false; };

    // initializer_list specialization (always contiguous)
    template <typename T>
    struct TIsContiguousContainer<std::initializer_list<T>>
    {
        static constexpr bool Value = true;
    };

    template <typename T>
    inline constexpr bool TIsContiguousContainer_V = TIsContiguousContainer<T>::Value;

} // namespace OloEngine

// STL container specializations (must be outside namespace for ADL to work)
#include <vector>
#include <array>
#include <string>
#include <string_view>

namespace OloEngine
{
    // std::vector specialization (always contiguous)
    template <typename T, typename Allocator>
    struct TIsContiguousContainer<std::vector<T, Allocator>>
    {
        static constexpr bool Value = true;
    };

    // std::array specialization (always contiguous)
    template <typename T, std::size_t N>
    struct TIsContiguousContainer<std::array<T, N>>
    {
        static constexpr bool Value = true;
    };

    // std::basic_string specialization (always contiguous)
    template <typename CharT, typename Traits, typename Allocator>
    struct TIsContiguousContainer<std::basic_string<CharT, Traits, Allocator>>
    {
        static constexpr bool Value = true;
    };

    // std::basic_string_view specialization (always contiguous)
    template <typename CharT, typename Traits>
    struct TIsContiguousContainer<std::basic_string_view<CharT, Traits>>
    {
        static constexpr bool Value = true;
    };

    // ========================================================================
    // Comparison Functors
    // ========================================================================

    // @struct TLess
    // @brief Binary predicate that returns true if the first argument is less than the second
    // 
    // Used as default predicate for sorting and heap operations.
    template <typename T = void>
    struct TLess
    {
        [[nodiscard]] constexpr bool operator()(const T& A, const T& B) const
        {
            return A < B;
        }
    };

    // Specialization for void - allows heterogeneous comparisons
    template <>
    struct TLess<void>
    {
        template <typename T, typename U>
        [[nodiscard]] constexpr bool operator()(T&& A, U&& B) const
        {
            return std::forward<T>(A) < std::forward<U>(B);
        }
    };

    // @struct TGreater
    // @brief Binary predicate that returns true if the first argument is greater than the second
    template <typename T = void>
    struct TGreater
    {
        [[nodiscard]] constexpr bool operator()(const T& A, const T& B) const
        {
            return A > B;
        }
    };

    // Specialization for void - allows heterogeneous comparisons
    template <>
    struct TGreater<void>
    {
        template <typename T, typename U>
        [[nodiscard]] constexpr bool operator()(T&& A, U&& B) const
        {
            return std::forward<T>(A) > std::forward<U>(B);
        }
    };

    // ========================================================================
    // CV-Qualifier Manipulation Traits
    // ========================================================================

    // @struct TCopyQualifiersFromTo
    // @brief Copies the cv-qualifiers from one type to another
    // 
    // Examples:
    // - TCopyQualifiersFromTo_T<const    T1,       T2> == const T2
    // - TCopyQualifiersFromTo_T<volatile T1, const T2> == const volatile T2
    template <typename From, typename To> struct TCopyQualifiersFromTo                          { using Type =                To; };
    template <typename From, typename To> struct TCopyQualifiersFromTo<const          From, To> { using Type = const          To; };
    template <typename From, typename To> struct TCopyQualifiersFromTo<      volatile From, To> { using Type =       volatile To; };
    template <typename From, typename To> struct TCopyQualifiersFromTo<const volatile From, To> { using Type = const volatile To; };

    template <typename From, typename To>
    using TCopyQualifiersFromTo_T = typename TCopyQualifiersFromTo<From, To>::Type;

    // @struct TLosesQualifiersFromTo
    // @brief Tests if qualifiers are lost between one type and another
     * 
     * Examples:
     * - TLosesQualifiersFromTo_V<const    T1,                T2> == true
     * - TLosesQualifiersFromTo_V<volatile T1, const volatile T2> == false
     */
    template <typename From, typename To>
    struct TLosesQualifiersFromTo
    {
        static constexpr bool Value = !std::is_same_v<TCopyQualifiersFromTo_T<From, To>, To>;
    };

    template <typename From, typename To>
    inline constexpr bool TLosesQualifiersFromTo_V = TLosesQualifiersFromTo<From, To>::Value;

} // namespace OloEngine

// ============================================================================
// Undef Macros abstracting the presence of certain compiler intrinsic type traits
// ============================================================================
#undef IS_EMPTY
#undef IS_POD
#undef HAS_TRIVIAL_CONSTRUCTOR
