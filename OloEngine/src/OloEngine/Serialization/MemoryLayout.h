#pragma once

/**
 * @file MemoryLayout.h
 * @brief Memory layout declarations for serialization and type layout
 * 
 * Provides macros for declaring type layouts used by the memory image system.
 * This is a simplified version of UE's MemoryLayout.h focused on the essential
 * type layout declaration macros.
 * 
 * Ported from Unreal Engine's Serialization/MemoryLayout.h
 */

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // ============================================================================
    // Type Layout Interface
    // ============================================================================

    namespace ETypeLayoutInterface
    {
        enum Type : u8
        {
            NonVirtual,
            Virtual,
            Abstract,
        };

        inline bool HasVTable(Type InType) { return InType != NonVirtual; }
    }

    // ============================================================================
    // Field Layout Flags
    // ============================================================================

    namespace EFieldLayoutFlags
    {
        enum Type : u8
        {
            None = 0u,
            WithEditorOnly = (1u << 0),
            WithRayTracing = (1u << 1),
            Transient = (1u << 2),
            UseInstanceWithNoProperty = (1u << 3),
        };

        OLO_FINLINE Type MakeFlags(u32 Flags = None) { return static_cast<Type>(Flags); }
        OLO_FINLINE Type MakeFlagsEditorOnly(u32 Flags = None) { return static_cast<Type>(WithEditorOnly | Flags); }
        OLO_FINLINE Type MakeFlagsRayTracing(u32 Flags = None) { return static_cast<Type>(WithRayTracing | Flags); }
    }

    // ============================================================================
    // THasTypeLayout - Trait to detect if a type has layout information
    // ============================================================================

    /**
     * @struct THasTypeLayout
     * @brief Trait that determines if a type has type layout information
     * 
     * By default, types don't have type layout. Specialize this for types
     * that participate in memory image serialization.
     */
    template <typename T>
    struct THasTypeLayout
    {
        static constexpr bool Value = false;
    };

    // Intrinsic types have type layout
    template <> struct THasTypeLayout<bool> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<i8> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<u8> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<i16> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<u16> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<i32> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<u32> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<i64> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<u64> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<f32> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<f64> { static constexpr bool Value = true; };

} // namespace OloEngine

// ============================================================================
// Type Layout Declaration Macros
// ============================================================================

/**
 * @def DECLARE_INTRINSIC_TYPE_LAYOUT
 * @brief Declares that a type is an intrinsic type with type layout
 * 
 * Use this for simple types like FSetElementId that are essentially just
 * wrappers around primitive types.
 */
#define DECLARE_INTRINSIC_TYPE_LAYOUT(T) \
    template <> struct OloEngine::THasTypeLayout<T> { static constexpr bool Value = true; }

/**
 * @def DECLARE_TEMPLATE_INTRINSIC_TYPE_LAYOUT
 * @brief Declares that a template type is an intrinsic type with type layout
 * 
 * Use this for template types that should be treated as intrinsic.
 * @param TemplatePrefix  The template declaration (e.g., template<typename T>)
 * @param T               The full type expression
 */
#define DECLARE_TEMPLATE_INTRINSIC_TYPE_LAYOUT(TemplatePrefix, T) \
    TemplatePrefix struct OloEngine::THasTypeLayout<T> { static constexpr bool Value = true; }

/**
 * @def DECLARE_INLINE_TYPE_LAYOUT
 * @brief Declares type layout for a class with inline field declarations
 * 
 * @param T              The type name
 * @param InterfaceType  One of: NonVirtual, Virtual, Abstract
 */
#define DECLARE_INLINE_TYPE_LAYOUT(T, InterfaceType) \
    /* Type layout metadata for T */ \
    static constexpr OloEngine::ETypeLayoutInterface::Type TypeLayoutInterface = OloEngine::ETypeLayoutInterface::InterfaceType

/**
 * @def LAYOUT_FIELD
 * @brief Declares a field in a type layout
 * 
 * Use within a class that has DECLARE_INLINE_TYPE_LAYOUT.
 */
#define LAYOUT_FIELD(Type, Name) Type Name

/**
 * @def LAYOUT_MUTABLE_FIELD
 * @brief Declares a mutable field in a type layout
 * 
 * Use within a class that has DECLARE_INLINE_TYPE_LAYOUT.
 */
#define LAYOUT_MUTABLE_FIELD(Type, Name) mutable Type Name

/**
 * @def LAYOUT_FIELD_INITIALIZED
 * @brief Declares a field with an initializer in a type layout
 */
#define LAYOUT_FIELD_INITIALIZED(Type, Name, Init) Type Name = Init

/**
 * @def LAYOUT_ARRAY
 * @brief Declares an array field in a type layout
 */
#define LAYOUT_ARRAY(Type, Name, Count) Type Name[Count]

/**
 * @def LAYOUT_BITFIELD
 * @brief Declares a bitfield in a type layout
 */
#define LAYOUT_BITFIELD(Type, Name, Bits) Type Name : Bits

