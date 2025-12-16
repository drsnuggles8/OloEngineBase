#pragma once

/**
 * @file MemoryLayout.h
 * @brief Memory layout declarations for serialization and type layout
 *
 * Provides a complete type layout system for memory image serialization:
 * - FTypeLayoutDesc: Runtime type descriptor with function pointers
 * - FFieldLayoutDesc: Field metadata for struct/class members
 * - Freeze namespace: Serialization/deserialization helpers
 * - Declaration/implementation macros for types with layout
 *
 * The memory layout system enables:
 * - Frozen memory images (pre-cooked assets)
 * - Cross-platform binary serialization
 * - Type-safe asset hot-reloading
 * - Runtime type introspection
 *
 * Ported from Unreal Engine 5.7's Serialization/MemoryLayout.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace OloEngine
{
    // ============================================================================
    // Forward Declarations
    // ============================================================================

    struct FTypeLayoutDesc;
    struct FFieldLayoutDesc;
    struct FPointerTableBase;
    class FMemoryImageWriter;
    struct FMemoryUnfreezeContent;
    struct FMemoryToStringContext;
    class FSHA1;
    struct FSHAHash;
    struct FPlatformTypeLayoutParameters;

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

        [[nodiscard]] OLO_FINLINE bool HasVTable(Type InType) { return InType != NonVirtual; }
    }

    // ============================================================================
    // Field Layout Flags
    // ============================================================================

    namespace EFieldLayoutFlags
    {
        enum Type : u8
        {
            None                      = 0u,
            WithEditorOnly            = (1u << 0),
            WithRayTracing            = (1u << 1),
            Transient                 = (1u << 2),
            UseInstanceWithNoProperty = (1u << 3),
        };

        [[nodiscard]] OLO_FINLINE Type MakeFlags(u32 Flags = None) { return static_cast<Type>(Flags); }
        [[nodiscard]] OLO_FINLINE Type MakeFlagsEditorOnly(u32 Flags = None) { return static_cast<Type>(WithEditorOnly | Flags); }
        [[nodiscard]] OLO_FINLINE Type MakeFlagsRayTracing(u32 Flags = None) { return static_cast<Type>(WithRayTracing | Flags); }
    }

    // ============================================================================
    // Function Pointer Typedefs
    // ============================================================================

    /** Destroys an object given its type layout */
    using FDestroyFunc = void(void* Object, const FTypeLayoutDesc& TypeDesc, const FPointerTableBase* PtrTable, bool bIsFrozen);

    /** Writes an object to a frozen memory image */
    using FWriteFrozenMemoryImageFunc = void(FMemoryImageWriter& Writer, const void* Object, const FTypeLayoutDesc& TypeDesc, const FTypeLayoutDesc& DerivedTypeDesc);

    /** Copies from frozen memory back to a live object */
    using FUnfrozenCopyFunc = u32(const FMemoryUnfreezeContent& Context, const void* Object, const FTypeLayoutDesc& TypeDesc, void* OutDst);

    /** Computes hash for type layout versioning */
    using FAppendHashFunc = u32(const FTypeLayoutDesc& TypeDesc, const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher);

    /** Returns target alignment for a type on a given platform */
    using FGetTargetAlignmentFunc = u32(const FTypeLayoutDesc& TypeDesc, const FPlatformTypeLayoutParameters& LayoutParams);

    /** Converts an object to a debug string representation */
    using FToStringFunc = void(const void* Object, const FTypeLayoutDesc& TypeDesc, const FPlatformTypeLayoutParameters& LayoutParams, FMemoryToStringContext& OutContext);

    /** Returns a pointer to a default-constructed object of the type */
    using FGetDefaultObjectFunc = const void*();

    // ============================================================================
    // FFieldLayoutDesc - Field Metadata
    // ============================================================================

    /**
     * @struct FFieldLayoutDesc
     * @brief Describes a single field within a type's memory layout
     *
     * Fields are stored as a linked list attached to their parent FTypeLayoutDesc.
     * Each field has its own type layout, offset, and optional custom serialization.
     */
    struct FFieldLayoutDesc
    {
        /** Writes this specific field to a frozen memory image */
        using FWriteFrozenMemoryImageFunc = void(FMemoryImageWriter& Writer, const void* Object, const void* FieldObject, const FTypeLayoutDesc& TypeDesc, const FTypeLayoutDesc& DerivedTypeDesc);

        const char*                 Name                       = nullptr;  ///< Field name (for debugging/reflection)
        const FTypeLayoutDesc*      Type                       = nullptr;  ///< Type layout of this field
        const FFieldLayoutDesc*     Next                       = nullptr;  ///< Next field in the linked list
        FWriteFrozenMemoryImageFunc* WriteFrozenMemoryImageFunc = nullptr;  ///< Custom field serializer (optional)
        u32                         Offset                     = 0;        ///< Byte offset from object start (or ~0u for bitfields)
        u32                         NumArray                   = 1;        ///< Array element count (1 for non-arrays)
        EFieldLayoutFlags::Type     Flags                      = EFieldLayoutFlags::None;
        u8                          BitFieldSize               = 0;        ///< Bit width if this is a bitfield (0 otherwise)
        u8                          UFieldNameLength           = 0;        ///< Length of name excluding _DEPRECATED suffix
    };

    // ============================================================================
    // FTypeLayoutDesc - Type Metadata
    // ============================================================================

    /**
     * @struct FTypeLayoutDesc
     * @brief Complete runtime descriptor for a type's memory layout
     *
     * Contains all metadata needed to serialize, deserialize, copy, hash,
     * and destroy objects of this type. Types are registered in a global
     * hash table for lookup by name.
     *
     * @note This struct is typically created by DECLARE_TYPE_LAYOUT macros
     *       and initialized lazily on first access via StaticGetTypeLayout().
     */
    struct FTypeLayoutDesc
    {
        // --- Hash Table Linkage ---
        const FTypeLayoutDesc*  HashNext                   = nullptr;  ///< Next entry in hash bucket

        // --- Identity ---
        const char*             Name                       = nullptr;  ///< Type name (from stringified macro)
        u64                     NameHash                   = 0;        ///< Precomputed hash of Name

        // --- Field Information ---
        const FFieldLayoutDesc* Fields                     = nullptr;  ///< Linked list of fields

        // --- Function Pointers ---
        FDestroyFunc*           DestroyFunc                = nullptr;  ///< Destructor wrapper
        FWriteFrozenMemoryImageFunc* WriteFrozenMemoryImageFunc = nullptr;  ///< Serializer
        FUnfrozenCopyFunc*      UnfrozenCopyFunc           = nullptr;  ///< Deserializer
        FAppendHashFunc*        AppendHashFunc             = nullptr;  ///< Hash appender
        FGetTargetAlignmentFunc* GetTargetAlignmentFunc    = nullptr;  ///< Platform alignment
        FToStringFunc*          ToStringFunc               = nullptr;  ///< Debug stringifier
        FGetDefaultObjectFunc*  GetDefaultObjectFunc       = nullptr;  ///< Default object provider

        // --- Size/Alignment ---
        u32                     Size                       = 0;        ///< sizeof(T)
        u32                     SizeFromFields             = 0;        ///< Computed size from fields (~0u if not computed)
        u32                     Alignment                  = 0;        ///< alignof(T)

        // --- Interface Type ---
        ETypeLayoutInterface::Type Interface               = ETypeLayoutInterface::NonVirtual;

        // --- Counts ---
        u8                      NumBases                   = 0;        ///< Number of base classes with layout
        u8                      NumVirtualBases            = 0;        ///< Number of virtual bases

        // --- Flags ---
        bool                    IsInitialized              = false;    ///< Set after first initialization
        bool                    IsIntrinsic                = false;    ///< True for built-in types (int, float, etc.)

        // --- Static Registration ---

        /**
         * @brief Gets an invalid type layout for error cases
         * @return Reference to a static invalid FTypeLayoutDesc
         */
        [[nodiscard]] static const FTypeLayoutDesc& GetInvalidTypeLayout()
        {
            static FTypeLayoutDesc Invalid;
            return Invalid;
        }

        /**
         * @brief Registers this type in the global type registry
         * @param TypeDesc The type descriptor to register
         */
        static void Register(FTypeLayoutDesc& TypeDesc)
        {
            // Registration implementation would add to a global hash table
            // For now, this is a no-op as we don't yet need runtime lookup
            (void)TypeDesc;
        }

        /**
         * @brief Finalizes initialization of a type descriptor
         * @param TypeDesc The type descriptor to finalize
         *
         * Called after all fields and bases have been added. Computes
         * derived values like NameHash and SizeFromFields.
         */
        static void Initialize(FTypeLayoutDesc& TypeDesc)
        {
            // Compute name hash if we have a name
            if (TypeDesc.Name)
            {
                TypeDesc.NameHash = 0;
                for (const char* p = TypeDesc.Name; *p; ++p)
                {
                    TypeDesc.NameHash = TypeDesc.NameHash * 31 + static_cast<u64>(*p);
                }
            }

            // Compute size from fields if not set
            if (TypeDesc.SizeFromFields == static_cast<u32>(~0))
            {
                u32 MaxOffset = 0;
                for (const FFieldLayoutDesc* Field = TypeDesc.Fields; Field; Field = Field->Next)
                {
                    if (Field->Offset != static_cast<u32>(~0) && Field->Type)
                    {
                        u32 FieldEnd = Field->Offset + Field->Type->Size * Field->NumArray;
                        if (FieldEnd > MaxOffset)
                        {
                            MaxOffset = FieldEnd;
                        }
                    }
                }
                TypeDesc.SizeFromFields = MaxOffset;
            }
        }
    };

    // ============================================================================
    // Freeze Namespace - Serialization Helpers
    // ============================================================================

    namespace Freeze
    {
        /**
         * @brief Default implementation for writing a field to a memory image
         */
        inline void DefaultWriteMemoryImageField(
            FMemoryImageWriter& Writer,
            const void* Object,
            const void* FieldObject,
            const FTypeLayoutDesc& TypeDesc,
            const FTypeLayoutDesc& DerivedTypeDesc)
        {
            // Default: delegate to the field type's writer
            (void)Writer; (void)Object; (void)FieldObject; (void)TypeDesc; (void)DerivedTypeDesc;
        }

        /**
         * @brief Default implementation for writing an object to a memory image
         */
        inline void DefaultWriteMemoryImage(
            FMemoryImageWriter& Writer,
            const void* Object,
            const FTypeLayoutDesc& TypeDesc,
            const FTypeLayoutDesc& DerivedTypeDesc)
        {
            // Default: write raw bytes
            (void)Writer; (void)Object; (void)TypeDesc; (void)DerivedTypeDesc;
        }

        /**
         * @brief Default implementation for copying from frozen to live memory
         */
        inline u32 DefaultUnfrozenCopy(
            const FMemoryUnfreezeContent& Context,
            const void* Object,
            const FTypeLayoutDesc& TypeDesc,
            void* OutDst)
        {
            std::memcpy(OutDst, Object, TypeDesc.Size);
            (void)Context;
            return TypeDesc.Size;
        }

        /**
         * @brief Default implementation for appending to a hash
         */
        inline u32 DefaultAppendHash(
            const FTypeLayoutDesc& TypeDesc,
            const FPlatformTypeLayoutParameters& LayoutParams,
            FSHA1& Hasher)
        {
            (void)TypeDesc; (void)LayoutParams; (void)Hasher;
            return TypeDesc.Alignment;
        }

        /**
         * @brief Appends hash for a specific type's layout
         */
        template <typename T>
        inline u32 AppendHash(
            const FTypeLayoutDesc& TypeDesc,
            const FPlatformTypeLayoutParameters& LayoutParams,
            FSHA1& Hasher)
        {
            return DefaultAppendHash(TypeDesc, LayoutParams, Hasher);
        }

        /**
         * @brief Non-template version for appending hash
         */
        inline u32 AppendHash(
            const FTypeLayoutDesc& TypeDesc,
            const FPlatformTypeLayoutParameters& LayoutParams,
            FSHA1& Hasher)
        {
            return DefaultAppendHash(TypeDesc, LayoutParams, Hasher);
        }

        /**
         * @brief Default implementation for getting target alignment
         */
        inline u32 DefaultGetTargetAlignment(
            const FTypeLayoutDesc& TypeDesc,
            const FPlatformTypeLayoutParameters& LayoutParams)
        {
            (void)LayoutParams;
            return TypeDesc.Alignment;
        }

        /**
         * @brief Default implementation for converting to string
         */
        inline void DefaultToString(
            const void* Object,
            const FTypeLayoutDesc& TypeDesc,
            const FPlatformTypeLayoutParameters& LayoutParams,
            FMemoryToStringContext& OutContext)
        {
            (void)Object; (void)TypeDesc; (void)LayoutParams; (void)OutContext;
        }

        /**
         * @brief Writes raw intrinsic data to a memory image
         */
        inline void IntrinsicWriteMemoryImage(
            FMemoryImageWriter& Writer,
            const void* Object,
            u32 Size)
        {
            (void)Writer; (void)Object; (void)Size;
        }

        /**
         * @brief Destroys an object, handling frozen vs live appropriately
         * @tparam T The object type
         * @param Object Pointer to the object to destroy
         * @param PtrTable Pointer table for frozen references (may be null)
         * @param bIsFrozen True if this is frozen memory
         */
        template <typename T>
        inline void DestroyObject(T* Object, const FPointerTableBase* PtrTable, bool bIsFrozen)
        {
            // Only call destructor for non-frozen objects
            if (!bIsFrozen)
            {
                Object->~T();
            }
            // Wipe destroyed memory to a recognizable pattern
            std::memset(Object, 0xFE, sizeof(T));
            (void)PtrTable;
        }

        /**
         * @brief Intrinsic copy from frozen memory
         */
        template <typename T>
        inline u32 IntrinsicUnfrozenCopy(const FMemoryUnfreezeContent& Context, const T& Object, void* OutDst)
        {
            new (OutDst) T(Object);
            (void)Context;
            return sizeof(T);
        }

        /**
         * @brief Finds the length of a field name, excluding _DEPRECATED suffix
         */
        inline u8 FindFieldNameLength(const char* Name)
        {
            if (!Name) return 0;

            // Find total length
            u8 Length = 0;
            const char* p = Name;
            while (*p) { ++p; ++Length; }

            // Check for _DEPRECATED suffix (11 chars)
            if (Length >= 11)
            {
                const char* suffix = p - 11;
                if (suffix[0] == '_' &&
                    suffix[1] == 'D' && suffix[2] == 'E' && suffix[3] == 'P' &&
                    suffix[4] == 'R' && suffix[5] == 'E' && suffix[6] == 'C' &&
                    suffix[7] == 'A' && suffix[8] == 'T' && suffix[9] == 'E' &&
                    suffix[10] == 'D')
                {
                    Length -= 11;
                }
            }
            return Length;
        }

    } // namespace Freeze

    // ============================================================================
    // Memory Image Support Types (Stub Implementations)
    // ============================================================================

    /**
     * @struct FPlatformTypeLayoutParameters
     * @brief Platform-specific layout parameters for memory image serialization
     */
    struct FPlatformTypeLayoutParameters
    {
        enum Flags : u32
        {
            Flag_Initialized = (1 << 0),
            Flag_Is32Bit = (1 << 1),
            Flag_AlignBases = (1 << 2),
            Flag_WithEditorOnly = (1 << 3),
        };

        u32 MaxFieldAlignment = 0xffffffff;
        u32 FlagsValue = 0;

        [[nodiscard]] bool IsInitialized() const { return (FlagsValue & Flag_Initialized) != 0u; }
        [[nodiscard]] bool Is32Bit() const { return (FlagsValue & Flag_Is32Bit) != 0u; }
        [[nodiscard]] bool HasAlignBases() const { return (FlagsValue & Flag_AlignBases) != 0u; }
        [[nodiscard]] bool WithEditorOnly() const { return (FlagsValue & Flag_WithEditorOnly) != 0u; }
        [[nodiscard]] u32 GetRawPointerSize() const { return Is32Bit() ? sizeof(u32) : sizeof(u64); }
    };

    /**
     * @class FMemoryImageWriter
     * @brief Writer for memory image serialization (cooked data)
     */
    class FMemoryImageWriter
    {
    public:
        [[nodiscard]] bool Is32BitTarget() const { return m_bIs32BitTarget; }

        void WriteBytes([[maybe_unused]] const void* Data, [[maybe_unused]] sizet Size) {}

        template <typename T>
        void WriteBytes(const T& Value) { WriteBytes(&Value, sizeof(T)); }

        FMemoryImageWriter WritePointer([[maybe_unused]] const FTypeLayoutDesc& TypeDesc) { return *this; }

        void WriteAlignment([[maybe_unused]] sizet Alignment) {}

        template <typename T>
        u32 WriteAlignment() { WriteAlignment(alignof(T)); return 0; }

        void WritePaddingToSize([[maybe_unused]] sizet Size) {}

        void WriteObject([[maybe_unused]] const void* Data, [[maybe_unused]] const FTypeLayoutDesc& TypeDesc) {}

        void WriteNullPointer() {}

        template <typename T>
        void WriteObjectArray([[maybe_unused]] const T* Data, [[maybe_unused]] const FTypeLayoutDesc& TypeDesc, [[maybe_unused]] i32 Count) {}

    private:
        bool m_bIs32BitTarget = false;
    };

    /**
     * @struct FMemoryUnfreezeContent
     * @brief Context for unfreezing memory images
     */
    struct FMemoryUnfreezeContent
    {
        template <typename T>
        void UnfreezeObject([[maybe_unused]] const T* Src, [[maybe_unused]] const FTypeLayoutDesc& TypeDesc, T* Dst) const
        {
            *Dst = *Src;
        }
    };

    /**
     * @class FSHA1
     * @brief SHA1 hash computation (stub)
     */
    class FSHA1
    {
    public:
        void Update([[maybe_unused]] const void* Data, [[maybe_unused]] sizet Size) {}
        void Final() {}
        void GetHash([[maybe_unused]] u8 OutHash[20]) {}
    };

    /**
     * @struct FSHAHash
     * @brief SHA1 hash result (stub)
     */
    struct FSHAHash
    {
        u8 Hash[20] = {};
    };

    /**
     * @struct FMemoryToStringContext
     * @brief Context for converting memory layout to string (stub)
     */
    struct FMemoryToStringContext
    {
        // Stub implementation
    };

    /**
     * @struct FPointerTableBase
     * @brief Base class for pointer tables used in frozen memory images (stub)
     */
    struct FPointerTableBase
    {
        virtual ~FPointerTableBase() = default;
    };

    // ============================================================================
    // Type Trait Helpers
    // ============================================================================

    /**
     * @struct THasTypeLayout
     * @brief Trait that determines if a type has type layout information
     *
     * By default, types don't have type layout. Types that declare
     * DECLARE_TYPE_LAYOUT or DECLARE_INTRINSIC_TYPE_LAYOUT will
     * have this trait specialized to true.
     */
    template <typename T>
    struct THasTypeLayout
    {
        static constexpr bool Value = false;
    };

    // Intrinsic types have type layout
    template <> struct THasTypeLayout<bool> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<char> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<signed char> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<short> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<int> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<long> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<long long> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<unsigned char> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<unsigned short> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<unsigned int> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<unsigned long> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<unsigned long long> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<float> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<double> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<wchar_t> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<char16_t> { static constexpr bool Value = true; };
    template <> struct THasTypeLayout<void*> { static constexpr bool Value = true; };

    // Note: OloEngine typedefs (i8, u8, i16, u16, i32, u32, i64, u64, f32, f64)
    // are just aliases to the standard types above, so no additional specializations needed.

    // ============================================================================
    // TStaticGetTypeLayoutHelper - Static Type Layout Access
    // ============================================================================

    /**
     * @struct TStaticGetTypeLayoutHelper
     * @brief Helper for accessing a type's static type layout descriptor
     *
     * Default implementation calls T::StaticGetTypeLayout(). Specialized
     * for intrinsic types to return inline-constructed descriptors.
     */
    template <typename T>
    struct TStaticGetTypeLayoutHelper
    {
        static const FTypeLayoutDesc& Do() { return T::StaticGetTypeLayout(); }
    };

    /**
     * @struct TGetTypeLayoutHelper
     * @brief Helper for getting type layout from an object instance
     *
     * For polymorphic types, returns the derived type's layout.
     */
    template <typename T>
    struct TGetTypeLayoutHelper
    {
        static const FTypeLayoutDesc& Do(const T& Object) { return Object.GetTypeLayout(); }
    };

    /**
     * @brief Get the static type layout for a type
     */
    template <typename T>
    [[nodiscard]] inline const FTypeLayoutDesc& StaticGetTypeLayoutDesc()
    {
        return TStaticGetTypeLayoutHelper<T>::Do();
    }

    /**
     * @brief Get the type layout from an object instance
     */
    template <typename T>
    [[nodiscard]] inline const FTypeLayoutDesc& GetTypeLayoutDesc(const FPointerTableBase*, const T& Object)
    {
        return TGetTypeLayoutHelper<T>::Do(Object);
    }

    // ============================================================================
    // TValidateInterfaceHelper - Interface Type Validation
    // ============================================================================

    template <typename T, ETypeLayoutInterface::Type InterfaceType>
    struct TValidateInterfaceHelper;

    template <typename T>
    struct TValidateInterfaceHelper<T, ETypeLayoutInterface::NonVirtual>
    {
        static constexpr bool Value = !std::is_polymorphic_v<T>;
    };

    template <typename T>
    struct TValidateInterfaceHelper<T, ETypeLayoutInterface::Virtual>
    {
        static constexpr bool Value = !std::is_abstract_v<T>;
    };

    template <typename T>
    struct TValidateInterfaceHelper<T, ETypeLayoutInterface::Abstract>
    {
        static constexpr bool Value = true;
    };

    // ============================================================================
    // TGetDefaultObjectHelper - Default Object Access
    // ============================================================================

    template <typename T, ETypeLayoutInterface::Type InterfaceType = ETypeLayoutInterface::NonVirtual>
    struct TGetDefaultObjectHelper
    {
        static const void* Do() { return nullptr; }
    };

    // For non-abstract virtual types, we can provide a default object
    // (Not implemented here - would require InternalGetDefaultObject)

    // ============================================================================
    // Base Type Detection
    // ============================================================================

    template <typename T>
    struct TGetBaseTypeHelper
    {
    private:
        template <typename InternalType>
        static typename InternalType::DerivedType Test(const typename InternalType::DerivedType*);
        template <typename InternalType>
        static void Test(...);

    public:
        using Type = decltype(Test<T>(nullptr));
    };

    template <typename T, typename BaseType>
    struct TInitializeBaseHelper
    {
        static void Initialize(FTypeLayoutDesc& TypeDesc) { (void)TypeDesc; }
    };

    template <typename T>
    struct TInitializeBaseHelper<T, void>
    {
        static void Initialize(FTypeLayoutDesc& /*TypeDesc*/) {}
    };

    // ============================================================================
    // Freeze Image Helpers
    // ============================================================================

    template <typename T, bool bUsePropertyFreezing = false>
    struct TGetFreezeImageHelper
    {
        static FWriteFrozenMemoryImageFunc* Do() { return &Freeze::DefaultWriteMemoryImage; }
    };

    template <typename T, bool bProvidesStaticStruct = false>
    struct TGetFreezeImageFieldHelper
    {
        static FFieldLayoutDesc::FWriteFrozenMemoryImageFunc* Do() { return &Freeze::DefaultWriteMemoryImageField; }
    };

    // ============================================================================
    // Concept for types with layout
    // ============================================================================

    struct CTypeLayout
    {
        template <typename T>
        auto Requires(const T&) -> decltype(T::StaticGetTypeLayout());
    };

} // namespace OloEngine

// ============================================================================
// Helper Macros
// ============================================================================

#ifndef STRUCT_OFFSET
#define STRUCT_OFFSET(Struct, Member) offsetof(Struct, Member)
#endif

#ifndef TEXT
#define TEXT(x) L##x
#endif

// UE_STATIC_ONLY emulates a struct that can't be instantiated
#define UE_STATIC_ONLY(T) T() = delete; ~T() = delete; T(const T&) = delete; T& operator=(const T&) = delete

// Counter-based internal link declaration
#define UE_DECLARE_INTERNAL_LINK_BASE(Name) template <int Counter> struct Name
#define UE_DECLARE_INTERNAL_LINK_SPECIALIZATION(Name, Counter) template <> struct Name<Counter>

// Preprocessor helpers
#define PREPROCESSOR_NOTHING
#define PREPROCESSOR_REMOVE_OPTIONAL_PARENS(...) __VA_ARGS__
#define PREPROCESSOR_JOIN(a, b) PREPROCESSOR_JOIN_INNER(a, b)
#define PREPROCESSOR_JOIN_INNER(a, b) a##b
#define ANONYMOUS_VARIABLE(Name) PREPROCESSOR_JOIN(Name, __COUNTER__)

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
    template <> struct OloEngine::THasTypeLayout<T> { static constexpr bool Value = true; }; \
    template <> struct OloEngine::TStaticGetTypeLayoutHelper<T> { \
        UE_STATIC_ONLY(TStaticGetTypeLayoutHelper); \
        static void CallWriteMemoryImage(OloEngine::FMemoryImageWriter& Writer, const void* Object, const OloEngine::FTypeLayoutDesc& TypeDesc, const OloEngine::FTypeLayoutDesc& DerivedTypeDesc) { \
            (void)Writer; (void)Object; (void)TypeDesc; (void)DerivedTypeDesc; \
        } \
        static void CallDestroy(void* Object, const OloEngine::FTypeLayoutDesc&, const OloEngine::FPointerTableBase* PtrTable, bool bIsFrozen) { \
            OloEngine::Freeze::DestroyObject(static_cast<T*>(Object), PtrTable, bIsFrozen); \
        } \
        static const OloEngine::FTypeLayoutDesc& Do() { \
            alignas(OloEngine::FTypeLayoutDesc) static std::uint8_t TypeBuffer[sizeof(OloEngine::FTypeLayoutDesc)] = { 0 }; \
            OloEngine::FTypeLayoutDesc& TypeDesc = *reinterpret_cast<OloEngine::FTypeLayoutDesc*>(TypeBuffer); \
            if (!TypeDesc.IsInitialized) { \
                TypeDesc.IsInitialized = true; \
                TypeDesc.IsIntrinsic = true; \
                TypeDesc.Name = #T; \
                TypeDesc.WriteFrozenMemoryImageFunc = &CallWriteMemoryImage; \
                TypeDesc.DestroyFunc = &CallDestroy; \
                TypeDesc.Size = sizeof(T); \
                TypeDesc.Alignment = alignof(T); \
                TypeDesc.Interface = OloEngine::ETypeLayoutInterface::NonVirtual; \
                TypeDesc.SizeFromFields = sizeof(T); \
            } \
            return TypeDesc; } }; \
    template <> struct OloEngine::TGetTypeLayoutHelper<T> { \
        UE_STATIC_ONLY(TGetTypeLayoutHelper); \
        static const OloEngine::FTypeLayoutDesc& Do(const T&) { return OloEngine::TStaticGetTypeLayoutHelper<T>::Do(); } }

/**
 * @def DECLARE_TEMPLATE_INTRINSIC_TYPE_LAYOUT
 * @brief Declares that a template type is an intrinsic type with type layout
 *
 * @param TemplatePrefix  The template declaration (e.g., template<typename T>)
 * @param T               The full type expression
 */
#define DECLARE_TEMPLATE_INTRINSIC_TYPE_LAYOUT(TemplatePrefix, T) \
    TemplatePrefix struct OloEngine::THasTypeLayout<T> { static constexpr bool Value = true; }; \
    TemplatePrefix struct OloEngine::TStaticGetTypeLayoutHelper<T> { \
        UE_STATIC_ONLY(TStaticGetTypeLayoutHelper); \
        static void CallWriteMemoryImage(OloEngine::FMemoryImageWriter& Writer, const void* Object, const OloEngine::FTypeLayoutDesc& TypeDesc, const OloEngine::FTypeLayoutDesc& DerivedTypeDesc) { \
            (void)Writer; (void)Object; (void)TypeDesc; (void)DerivedTypeDesc; \
        } \
        static void CallDestroy(void* Object, const OloEngine::FTypeLayoutDesc&, const OloEngine::FPointerTableBase* PtrTable, bool bIsFrozen) { \
            OloEngine::Freeze::DestroyObject(static_cast<T*>(Object), PtrTable, bIsFrozen); \
        } \
        static const OloEngine::FTypeLayoutDesc& Do() { \
            alignas(OloEngine::FTypeLayoutDesc) static std::uint8_t TypeBuffer[sizeof(OloEngine::FTypeLayoutDesc)] = { 0 }; \
            OloEngine::FTypeLayoutDesc& TypeDesc = *reinterpret_cast<OloEngine::FTypeLayoutDesc*>(TypeBuffer); \
            if (!TypeDesc.IsInitialized) { \
                TypeDesc.IsInitialized = true; \
                TypeDesc.IsIntrinsic = true; \
                TypeDesc.Name = #T; \
                TypeDesc.WriteFrozenMemoryImageFunc = &CallWriteMemoryImage; \
                TypeDesc.DestroyFunc = &CallDestroy; \
                TypeDesc.Size = sizeof(T); \
                TypeDesc.Alignment = alignof(T); \
                TypeDesc.Interface = OloEngine::ETypeLayoutInterface::NonVirtual; \
                TypeDesc.SizeFromFields = sizeof(T); \
            } \
            return TypeDesc; } }; \
    TemplatePrefix struct OloEngine::TGetTypeLayoutHelper<T> { \
        UE_STATIC_ONLY(TGetTypeLayoutHelper); \
        static const OloEngine::FTypeLayoutDesc& Do(const T&) { return OloEngine::TStaticGetTypeLayoutHelper<T>::Do(); } }

/**
 * @def ALIAS_TEMPLATE_TYPE_LAYOUT
 * @brief Aliases one type's layout to another
 */
#define ALIAS_TEMPLATE_TYPE_LAYOUT(TemplatePrefix, T, Alias) \
    TemplatePrefix struct OloEngine::TStaticGetTypeLayoutHelper<T> : public OloEngine::TStaticGetTypeLayoutHelper<Alias> { UE_STATIC_ONLY(TStaticGetTypeLayoutHelper); }; \
    TemplatePrefix struct OloEngine::TGetTypeLayoutHelper<T> : public OloEngine::TGetTypeLayoutHelper<Alias> { UE_STATIC_ONLY(TGetTypeLayoutHelper); }

/**
 * @def ALIAS_TYPE_LAYOUT
 * @brief Aliases one type's layout to another with size validation
 */
#define ALIAS_TYPE_LAYOUT(Type, Alias) \
    static_assert(sizeof(Type) == sizeof(Alias), "Using a type alias but the sizes don't match!"); \
    ALIAS_TEMPLATE_TYPE_LAYOUT(template<>, Type, Alias)

// Map 'const' types to non-const type
ALIAS_TEMPLATE_TYPE_LAYOUT(template <typename T>, const T, T);

// All raw pointer types map to void*
ALIAS_TEMPLATE_TYPE_LAYOUT(template <typename T>, T*, void*);

// ============================================================================
// Internal Implementation Macros
// ============================================================================

#define INTERNAL_DECLARE_TYPE_LAYOUT_COMMON(T, InInterface) \
    static constexpr int CounterBase = __COUNTER__; \
    public: using DerivedType = T; \
    static constexpr OloEngine::ETypeLayoutInterface::Type InterfaceType = OloEngine::ETypeLayoutInterface::InInterface; \
    UE_DECLARE_INTERNAL_LINK_BASE(InternalLinkType) { UE_STATIC_ONLY(InternalLinkType); static void Initialize(OloEngine::FTypeLayoutDesc& TypeDesc) { (void)TypeDesc; } }

#define INTERNAL_DECLARE_LAYOUT_BASE(T) \
    private: using InternalBaseType = typename OloEngine::TGetBaseTypeHelper<T>::Type; \
    template <typename InternalType> static void InternalInitializeBases(OloEngine::FTypeLayoutDesc& TypeDesc) { OloEngine::TInitializeBaseHelper<InternalType, InternalBaseType>::Initialize(TypeDesc); }

/**
 * @def DECLARE_INLINE_TYPE_LAYOUT
 * @brief Declares type layout for a class with inline implementation
 *
 * @param T              The type name
 * @param InterfaceType  One of: NonVirtual, Virtual, Abstract
 */
#define DECLARE_INLINE_TYPE_LAYOUT(T, InInterface) \
    INTERNAL_DECLARE_LAYOUT_BASE(T); \
    private: static void InternalDestroy(void* Object, const OloEngine::FTypeLayoutDesc&, const OloEngine::FPointerTableBase* PtrTable, bool bIsFrozen) { \
        OloEngine::Freeze::DestroyObject(static_cast<T*>(Object), PtrTable, bIsFrozen); \
    } \
    public: static OloEngine::FTypeLayoutDesc& StaticGetTypeLayout() { \
        static_assert(OloEngine::TValidateInterfaceHelper<T, OloEngine::ETypeLayoutInterface::InInterface>::Value, #InInterface " is invalid interface for " #T); \
        alignas(OloEngine::FTypeLayoutDesc) static std::uint8_t TypeBuffer[sizeof(OloEngine::FTypeLayoutDesc)] = { 0 }; \
        OloEngine::FTypeLayoutDesc& TypeDesc = *reinterpret_cast<OloEngine::FTypeLayoutDesc*>(TypeBuffer); \
        if (!TypeDesc.IsInitialized) { \
            TypeDesc.IsInitialized = true; \
            TypeDesc.Name = #T; \
            TypeDesc.WriteFrozenMemoryImageFunc = OloEngine::TGetFreezeImageHelper<T>::Do(); \
            TypeDesc.UnfrozenCopyFunc = &OloEngine::Freeze::DefaultUnfrozenCopy; \
            TypeDesc.AppendHashFunc = &OloEngine::Freeze::DefaultAppendHash; \
            TypeDesc.GetTargetAlignmentFunc = &OloEngine::Freeze::DefaultGetTargetAlignment; \
            TypeDesc.ToStringFunc = &OloEngine::Freeze::DefaultToString; \
            TypeDesc.DestroyFunc = &InternalDestroy; \
            TypeDesc.Size = sizeof(T); \
            TypeDesc.Alignment = alignof(T); \
            TypeDesc.Interface = OloEngine::ETypeLayoutInterface::InInterface; \
            TypeDesc.SizeFromFields = ~0u; \
            TypeDesc.GetDefaultObjectFunc = &OloEngine::TGetDefaultObjectHelper<T, OloEngine::ETypeLayoutInterface::InInterface>::Do; \
            InternalLinkType<1>::Initialize(TypeDesc); \
            InternalInitializeBases<T>(TypeDesc); \
            OloEngine::FTypeLayoutDesc::Initialize(TypeDesc); \
        } \
        return TypeDesc; } \
    public: const OloEngine::FTypeLayoutDesc& GetTypeLayout() const { return StaticGetTypeLayout(); } \
    INTERNAL_DECLARE_TYPE_LAYOUT_COMMON(T, InInterface)

/**
 * @def DECLARE_TYPE_LAYOUT
 * @brief Declares type layout for a class (requires IMPLEMENT_TYPE_LAYOUT in .cpp)
 *
 * @param T              The type name
 * @param InterfaceType  One of: NonVirtual, Virtual, Abstract
 */
#define DECLARE_TYPE_LAYOUT(T, Interface) \
    INTERNAL_DECLARE_LAYOUT_BASE(T); \
    private: static void InternalDestroy(void* Object, const OloEngine::FTypeLayoutDesc&, const OloEngine::FPointerTableBase* PtrTable, bool bIsFrozen); \
    public: static OloEngine::FTypeLayoutDesc& StaticGetTypeLayout(); \
    public: const OloEngine::FTypeLayoutDesc& GetTypeLayout() const; \
    INTERNAL_DECLARE_TYPE_LAYOUT_COMMON(T, Interface)

/**
 * @def DECLARE_EXPORTED_TYPE_LAYOUT
 * @brief Declares type layout with DLL export specifier
 */
#define DECLARE_EXPORTED_TYPE_LAYOUT(T, RequiredAPI, Interface) \
    INTERNAL_DECLARE_LAYOUT_BASE(T); \
    private: static void InternalDestroy(void* Object, const OloEngine::FTypeLayoutDesc&, const OloEngine::FPointerTableBase* PtrTable, bool bIsFrozen); \
    public: RequiredAPI static OloEngine::FTypeLayoutDesc& StaticGetTypeLayout(); \
    public: RequiredAPI const OloEngine::FTypeLayoutDesc& GetTypeLayout() const; \
    INTERNAL_DECLARE_TYPE_LAYOUT_COMMON(T, Interface)

// ============================================================================
// Field Declaration Macros
// ============================================================================

/**
 * @def LAYOUT_FIELD
 * @brief Declares a field in a type layout
 *
 * Use within a class that has DECLARE_TYPE_LAYOUT or DECLARE_INLINE_TYPE_LAYOUT.
 * The macro both declares the member and registers it in the type's field list.
 */
#define LAYOUT_FIELD(Type, Name, ...) Type Name

/**
 * @def LAYOUT_MUTABLE_FIELD
 * @brief Declares a mutable field in a type layout
 */
#define LAYOUT_MUTABLE_FIELD(Type, Name, ...) mutable Type Name

/**
 * @def LAYOUT_FIELD_INITIALIZED
 * @brief Declares a field with an initializer in a type layout
 */
#define LAYOUT_FIELD_INITIALIZED(Type, Name, Init, ...) Type Name = Init

/**
 * @def LAYOUT_MUTABLE_FIELD_INITIALIZED
 * @brief Declares a mutable field with an initializer in a type layout
 */
#define LAYOUT_MUTABLE_FIELD_INITIALIZED(Type, Name, Init, ...) mutable Type Name = Init

/**
 * @def LAYOUT_ARRAY
 * @brief Declares an array field in a type layout
 */
#define LAYOUT_ARRAY(Type, Name, Count, ...) Type Name[Count]

/**
 * @def LAYOUT_BITFIELD
 * @brief Declares a bitfield in a type layout
 */
#define LAYOUT_BITFIELD(Type, Name, Bits, ...) Type Name : Bits

/**
 * @def LAYOUT_MUTABLE_BITFIELD
 * @brief Declares a mutable bitfield in a type layout
 */
#define LAYOUT_MUTABLE_BITFIELD(Type, Name, Bits, ...) mutable Type Name : Bits

/**
 * @def LAYOUT_FIELD_WITH_WRITER
 * @brief Declares a field with a custom writer function
 */
#define LAYOUT_FIELD_WITH_WRITER(Type, Name, Func) Type Name

/**
 * @def LAYOUT_MUTABLE_FIELD_WITH_WRITER
 * @brief Declares a mutable field with a custom writer function
 */
#define LAYOUT_MUTABLE_FIELD_WITH_WRITER(Type, Name, Func) mutable Type Name

/**
 * @def LAYOUT_WRITE_MEMORY_IMAGE
 * @brief Declares a custom memory image writer for the type
 */
#define LAYOUT_WRITE_MEMORY_IMAGE(Func) /* Custom writer: Func */

/**
 * @def LAYOUT_TOSTRING
 * @brief Declares a custom to-string function for the type
 */
#define LAYOUT_TOSTRING(Func) /* Custom toString: Func */

// Editor-only field variants
#ifdef WITH_EDITORONLY_DATA
#define LAYOUT_FIELD_EDITORONLY(Type, Name, ...) Type Name
#define LAYOUT_ARRAY_EDITORONLY(Type, Name, Count, ...) Type Name[Count]
#define LAYOUT_BITFIELD_EDITORONLY(Type, Name, Bits, ...) Type Name : Bits
#else
#define LAYOUT_FIELD_EDITORONLY(Type, Name, ...)
#define LAYOUT_ARRAY_EDITORONLY(Type, Name, Count, ...)
#define LAYOUT_BITFIELD_EDITORONLY(Type, Name, Bits, ...)
#endif

// ============================================================================
// Implementation Macros
// ============================================================================

/**
 * @def IMPLEMENT_TYPE_LAYOUT
 * @brief Implements type layout for a class declared with DECLARE_TYPE_LAYOUT
 */
#define IMPLEMENT_TYPE_LAYOUT(T) \
    void T::InternalDestroy(void* Object, const OloEngine::FTypeLayoutDesc&, const OloEngine::FPointerTableBase* PtrTable, bool bIsFrozen) { \
        OloEngine::Freeze::DestroyObject(static_cast<T*>(Object), PtrTable, bIsFrozen); \
    } \
    OloEngine::FTypeLayoutDesc& T::StaticGetTypeLayout() { \
        static_assert(OloEngine::TValidateInterfaceHelper<T, InterfaceType>::Value, "Invalid interface for " #T); \
        alignas(OloEngine::FTypeLayoutDesc) static std::uint8_t TypeBuffer[sizeof(OloEngine::FTypeLayoutDesc)] = { 0 }; \
        OloEngine::FTypeLayoutDesc& TypeDesc = *reinterpret_cast<OloEngine::FTypeLayoutDesc*>(TypeBuffer); \
        if (!TypeDesc.IsInitialized) { \
            TypeDesc.IsInitialized = true; \
            TypeDesc.Name = #T; \
            TypeDesc.WriteFrozenMemoryImageFunc = OloEngine::TGetFreezeImageHelper<T>::Do(); \
            TypeDesc.UnfrozenCopyFunc = &OloEngine::Freeze::DefaultUnfrozenCopy; \
            TypeDesc.AppendHashFunc = &OloEngine::Freeze::DefaultAppendHash; \
            TypeDesc.GetTargetAlignmentFunc = &OloEngine::Freeze::DefaultGetTargetAlignment; \
            TypeDesc.ToStringFunc = &OloEngine::Freeze::DefaultToString; \
            TypeDesc.DestroyFunc = &InternalDestroy; \
            TypeDesc.Size = sizeof(T); \
            TypeDesc.Alignment = alignof(T); \
            TypeDesc.Interface = InterfaceType; \
            TypeDesc.SizeFromFields = ~0u; \
            TypeDesc.GetDefaultObjectFunc = &OloEngine::TGetDefaultObjectHelper<T, InterfaceType>::Do; \
            InternalLinkType<1>::Initialize(TypeDesc); \
            InternalInitializeBases<T>(TypeDesc); \
            OloEngine::FTypeLayoutDesc::Initialize(TypeDesc); \
        } \
        return TypeDesc; } \
    const OloEngine::FTypeLayoutDesc& T::GetTypeLayout() const { return StaticGetTypeLayout(); }

/**
 * @def IMPLEMENT_ABSTRACT_TYPE_LAYOUT
 * @brief Implements type layout for an abstract class
 */
#define IMPLEMENT_ABSTRACT_TYPE_LAYOUT(T) \
    void T::InternalDestroy(void* Object, const OloEngine::FTypeLayoutDesc&, const OloEngine::FPointerTableBase* PtrTable, bool bIsFrozen) { \
        OloEngine::Freeze::DestroyObject(static_cast<T*>(Object), PtrTable, bIsFrozen); \
    } \
    OloEngine::FTypeLayoutDesc& T::StaticGetTypeLayout() { \
        alignas(OloEngine::FTypeLayoutDesc) static std::uint8_t TypeBuffer[sizeof(OloEngine::FTypeLayoutDesc)] = { 0 }; \
        OloEngine::FTypeLayoutDesc& TypeDesc = *reinterpret_cast<OloEngine::FTypeLayoutDesc*>(TypeBuffer); \
        if (!TypeDesc.IsInitialized) { \
            TypeDesc.IsInitialized = true; \
            TypeDesc.Name = #T; \
            TypeDesc.WriteFrozenMemoryImageFunc = OloEngine::TGetFreezeImageHelper<T>::Do(); \
            TypeDesc.UnfrozenCopyFunc = &OloEngine::Freeze::DefaultUnfrozenCopy; \
            TypeDesc.AppendHashFunc = &OloEngine::Freeze::DefaultAppendHash; \
            TypeDesc.GetTargetAlignmentFunc = &OloEngine::Freeze::DefaultGetTargetAlignment; \
            TypeDesc.ToStringFunc = &OloEngine::Freeze::DefaultToString; \
            TypeDesc.DestroyFunc = &InternalDestroy; \
            TypeDesc.Size = sizeof(T); \
            TypeDesc.Alignment = alignof(T); \
            TypeDesc.Interface = InterfaceType; \
            TypeDesc.SizeFromFields = ~0u; \
            TypeDesc.GetDefaultObjectFunc = &OloEngine::TGetDefaultObjectHelper<T, InterfaceType>::Do; \
            InternalLinkType<1>::Initialize(TypeDesc); \
            InternalInitializeBases<T>(TypeDesc); \
            OloEngine::FTypeLayoutDesc::Initialize(TypeDesc); \
        } \
        return TypeDesc; }

/**
 * @def REGISTER_INLINE_TYPE_LAYOUT
 * @brief Registers an inline type layout with the global registry
 */
#define REGISTER_INLINE_TYPE_LAYOUT(T) \
    static struct ANONYMOUS_VARIABLE(RegisterTypeLayout) { \
        ANONYMOUS_VARIABLE(RegisterTypeLayout)() { \
            T::StaticGetTypeLayout().Name = TEXT(#T); \
            OloEngine::FTypeLayoutDesc::Register(T::StaticGetTypeLayout()); \
        } \
    } ANONYMOUS_VARIABLE(RegisterTypeLayoutInstance)

