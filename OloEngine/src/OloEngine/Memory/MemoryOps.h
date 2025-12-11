#pragma once

/**
 * @file MemoryOps.h
 * @brief Memory operation templates for efficient element construction, destruction, and relocation
 * 
 * Provides optimized memory operations for containers that leverage type traits to use
 * memcpy/memset when safe, falling back to proper constructors/destructors otherwise.
 * 
 * Key operations:
 * - DefaultConstructItems: Default construct a range of elements
 * - DestructItem(s): Destruct one or more elements
 * - ConstructItems: Copy construct elements from source
 * - CopyAssignItems: Copy assign elements
 * - RelocateConstructItem(s): Move elements to new location (destructive move)
 * - MoveConstructItems: Move construct elements
 * - MoveAssignItems: Move assign elements
 * - CompareItems: Compare element ranges
 * 
 * Ported from Unreal Engine's Templates/MemoryOps.h
 */

#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include <new>
#include <type_traits>

namespace OloEngine
{
    // ============================================================================
    // Internal helper for bitwise relocation decision
    // ============================================================================
    namespace Detail
    {
        template <typename DestinationElementType, typename SourceElementType>
        inline constexpr bool TCanBitwiseRelocate_V =
            std::is_same_v<DestinationElementType, SourceElementType> || (
                TIsBitwiseConstructible<DestinationElementType, SourceElementType>::Value &&
                std::is_trivially_destructible_v<SourceElementType>
            );
    }

    // ============================================================================
    // Default Construction
    // ============================================================================

    /**
     * @brief Default constructs a range of items in memory (zero-constructible types)
     * 
     * For types where TIsZeroConstructType is true, this uses memset for efficiency.
     * 
     * @param Address   The address of the first memory location to construct at
     * @param Count     The number of elements to construct
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && TIsZeroConstructType<ElementType>::Value)
    OLO_FINLINE void DefaultConstructItems(void* Address, SizeType Count)
    {
        FMemory::Memset(Address, 0, sizeof(ElementType) * Count);
    }

    /**
     * @brief Default constructs a range of items in memory (non-zero-constructible types)
     * 
     * For types that require proper construction, this calls the default constructor for each element.
     * 
     * @param Address   The address of the first memory location to construct at
     * @param Count     The number of elements to construct
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && !TIsZeroConstructType<ElementType>::Value)
    OLO_NOINLINE void DefaultConstructItems(void* Address, SizeType Count)
    {
        ElementType* Element = static_cast<ElementType*>(Address);
        while (Count)
        {
            ::new (static_cast<void*>(Element)) ElementType;
            ++Element;
            --Count;
        }
    }

    // ============================================================================
    // Destruction
    // ============================================================================

    /**
     * @brief Destructs a single item in memory
     * 
     * @param Element   A pointer to the item to destruct
     * 
     * @note This function is optimized for values of T, and so will not dynamically 
     *       dispatch destructor calls if T's destructor is virtual.
     */
    template <typename ElementType>
    OLO_FINLINE constexpr void DestructItem(ElementType* Element)
    {
        if constexpr (sizeof(ElementType) == 0)
        {
            // Should never get here, but this construct improves error messages for incomplete types
        }
        else if constexpr (!std::is_trivially_destructible_v<ElementType>)
        {
            // We need a typedef here because VC won't compile the destructor call below 
            // if ElementType itself has a member called ElementType
            typedef ElementType DestructItemsElementTypeTypedef;

            Element->DestructItemsElementTypeTypedef::~DestructItemsElementTypeTypedef();
        }
    }

    /**
     * @brief Destructs a range of items in memory (trivially destructible types - no-op)
     * 
     * @param Element   A pointer to the first item to destruct
     * @param Count     The number of elements to destruct
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && std::is_trivially_destructible_v<ElementType>)
    OLO_FINLINE constexpr void DestructItems([[maybe_unused]] ElementType* Element, [[maybe_unused]] SizeType Count)
    {
        // Trivially destructible - nothing to do
    }

    /**
     * @brief Destructs a range of items in memory (non-trivially destructible types)
     * 
     * @param Element   A pointer to the first item to destruct
     * @param Count     The number of elements to destruct
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && !std::is_trivially_destructible_v<ElementType>)
    OLO_NOINLINE constexpr void DestructItems(ElementType* Element, SizeType Count)
    {
        while (Count)
        {
            // We need a typedef here because VC won't compile the destructor call below 
            // if ElementType itself has a member called ElementType
            typedef ElementType DestructItemsElementTypeTypedef;

            Element->DestructItemsElementTypeTypedef::~DestructItemsElementTypeTypedef();
            ++Element;
            --Count;
        }
    }

    // ============================================================================
    // Copy Construction
    // ============================================================================

    /**
     * @brief Constructs a range of items into memory from another array (bitwise constructible)
     * 
     * @param Dest      The memory location to start copying into
     * @param Source    A pointer to the first argument to pass to the constructor
     * @param Count     The number of elements to copy
     */
    template <typename DestinationElementType, typename SourceElementType, typename SizeType>
        requires (sizeof(DestinationElementType) > 0 && sizeof(SourceElementType) > 0 && 
                  TIsBitwiseConstructible_V<DestinationElementType, SourceElementType>)
    OLO_FINLINE void ConstructItems(void* Dest, const SourceElementType* Source, SizeType Count)
    {
        if (Count)
        {
            FMemory::Memcpy(Dest, Source, sizeof(SourceElementType) * Count);
        }
    }

    /**
     * @brief Constructs a range of items into memory from another array (non-bitwise constructible)
     * 
     * @param Dest      The memory location to start copying into
     * @param Source    A pointer to the first argument to pass to the constructor
     * @param Count     The number of elements to copy
     */
    template <typename DestinationElementType, typename SourceElementType, typename SizeType>
        requires (sizeof(DestinationElementType) > 0 && sizeof(SourceElementType) > 0 && 
                  !TIsBitwiseConstructible_V<DestinationElementType, SourceElementType>)
    OLO_NOINLINE void ConstructItems(void* Dest, const SourceElementType* Source, SizeType Count)
    {
        while (Count)
        {
            ::new (static_cast<void*>(Dest)) DestinationElementType(*Source);
            ++(DestinationElementType*&)Dest;
            ++Source;
            --Count;
        }
    }

    // ============================================================================
    // Copy Assignment
    // ============================================================================

    /**
     * @brief Copy assigns a range of items (trivially copy assignable)
     * 
     * @param Dest      The memory location to start assigning to
     * @param Source    A pointer to the first item to assign
     * @param Count     The number of elements to assign
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && std::is_trivially_copy_assignable_v<ElementType>)
    OLO_FINLINE void CopyAssignItems(ElementType* Dest, const ElementType* Source, SizeType Count)
    {
        FMemory::Memcpy(Dest, Source, sizeof(ElementType) * Count);
    }

    /**
     * @brief Copy assigns a range of items (non-trivially copy assignable)
     * 
     * @param Dest      The memory location to start assigning to
     * @param Source    A pointer to the first item to assign
     * @param Count     The number of elements to assign
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && !std::is_trivially_copy_assignable_v<ElementType>)
    OLO_NOINLINE void CopyAssignItems(ElementType* Dest, const ElementType* Source, SizeType Count)
    {
        while (Count)
        {
            *Dest = *Source;
            ++Dest;
            ++Source;
            --Count;
        }
    }

    // ============================================================================
    // Relocation (Destructive Move)
    // ============================================================================

    /**
     * @brief Relocates a single item to a new memory location as a new type
     * 
     * This is a 'destructive move' for which there is no single operation in C++ 
     * but which can be implemented very efficiently in general.
     * 
     * @param Dest      The memory location to relocate to
     * @param Source    A pointer to the item to relocate
     * 
     * @note For single items, we check TUseBitwiseSwap to avoid using memcpy for
     *       small 'register' types (pointers, ints, floats) where register moves are faster.
     *       For bulk operations, memcpy is always better due to loop overhead.
     */
    template <typename DestinationElementType, typename SourceElementType>
    OLO_FINLINE void RelocateConstructItem(void* Dest, SourceElementType* Source)
    {
        if constexpr (sizeof(DestinationElementType) == 0 || sizeof(SourceElementType) == 0)
        {
            // Should never get here, but this construct improves error messages for incomplete types
        }
        // Do a bitwise relocate if TCanBitwiseRelocate_V says we can, but not if the type
        // is known not to benefit from bitwise swap (small register types)
        else if constexpr (Detail::TCanBitwiseRelocate_V<DestinationElementType, SourceElementType> && 
                          (!std::is_same_v<DestinationElementType, SourceElementType> || TUseBitwiseSwap<SourceElementType>::Value))
        {
            // Bitwise relocate - memcpy is sufficient for larger types or cross-type relocations
            FMemory::Memmove(Dest, Source, sizeof(SourceElementType));
        }
        else
        {
            // We need a typedef here because VC won't compile the destructor call below 
            // if SourceElementType itself has a member called SourceElementType
            typedef SourceElementType RelocateConstructItemsElementTypeTypedef;

            ::new (static_cast<void*>(Dest)) DestinationElementType(std::move(*Source));
            Source->RelocateConstructItemsElementTypeTypedef::~RelocateConstructItemsElementTypeTypedef();
        }
    }

    /**
     * @brief Relocates a range of items to a new memory location (bitwise relocatable)
     * 
     * @param Dest      The memory location to relocate to
     * @param Source    A pointer to the first item to relocate
     * @param Count     The number of elements to relocate
     */
    template <typename DestinationElementType, typename SourceElementType, typename SizeType>
        requires (sizeof(DestinationElementType) > 0 && sizeof(SourceElementType) > 0 && 
                  Detail::TCanBitwiseRelocate_V<DestinationElementType, SourceElementType>)
    OLO_FINLINE void RelocateConstructItems(void* Dest, SourceElementType* Source, SizeType Count)
    {
        static_assert(!std::is_const_v<SourceElementType>, "RelocateConstructItems: Source cannot be const");

        /* All existing UE containers seem to assume trivial relocatability (i.e. memcpy'able) of their members,
         * so we're going to assume that this is safe here. However, it's not generally possible to assume this
         * in general as objects which contain pointers/references to themselves are not safe to be trivially
         * relocated.
         *
         * However, it is not yet possible to automatically infer this at compile time, so we can't enable
         * different (i.e. safer) implementations anyway. */

        FMemory::Memmove(Dest, Source, sizeof(SourceElementType) * Count);
    }

    /**
     * @brief Relocates a range of items to a new memory location (non-bitwise relocatable)
     * 
     * @param Dest      The memory location to relocate to
     * @param Source    A pointer to the first item to relocate
     * @param Count     The number of elements to relocate
     */
    template <typename DestinationElementType, typename SourceElementType, typename SizeType>
        requires (sizeof(DestinationElementType) > 0 && sizeof(SourceElementType) > 0 && 
                  !Detail::TCanBitwiseRelocate_V<DestinationElementType, SourceElementType>)
    OLO_NOINLINE void RelocateConstructItems(void* Dest, SourceElementType* Source, SizeType Count)
    {
        static_assert(!std::is_const_v<SourceElementType>, "RelocateConstructItems: Source cannot be const");

        while (Count)
        {
            // We need a typedef here because VC won't compile the destructor call below 
            // if SourceElementType itself has a member called SourceElementType
            typedef SourceElementType RelocateConstructItemsElementTypeTypedef;

            ::new (static_cast<void*>(Dest)) DestinationElementType(static_cast<SourceElementType&&>(*Source));
            ++(DestinationElementType*&)Dest;
            (Source++)->RelocateConstructItemsElementTypeTypedef::~RelocateConstructItemsElementTypeTypedef();
            --Count;
        }
    }

    // ============================================================================
    // Move Construction
    // ============================================================================

    /**
     * @brief Move constructs a range of items into memory (trivially copy constructible)
     * 
     * @param Dest      The memory location to start moving into
     * @param Source    A pointer to the first item to move from
     * @param Count     The number of elements to move
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && std::is_trivially_copy_constructible_v<ElementType>)
    OLO_FINLINE void MoveConstructItems(void* Dest, const ElementType* Source, SizeType Count)
    {
        FMemory::Memmove(Dest, Source, sizeof(ElementType) * Count);
    }

    /**
     * @brief Move constructs a range of items into memory (non-trivially copy constructible)
     * 
     * @param Dest      The memory location to start moving into
     * @param Source    A pointer to the first item to move from
     * @param Count     The number of elements to move
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && !std::is_trivially_copy_constructible_v<ElementType>)
    OLO_NOINLINE void MoveConstructItems(void* Dest, const ElementType* Source, SizeType Count)
    {
        while (Count)
        {
            ::new (static_cast<void*>(Dest)) ElementType(static_cast<ElementType&&>(*const_cast<ElementType*>(Source)));
            ++(ElementType*&)Dest;
            ++Source;
            --Count;
        }
    }

    // ============================================================================
    // Move Assignment
    // ============================================================================

    /**
     * @brief Move assigns a range of items (trivially copy assignable)
     * 
     * @param Dest      The memory location to start move assigning to
     * @param Source    A pointer to the first item to move assign
     * @param Count     The number of elements to move assign
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && std::is_trivially_copy_assignable_v<ElementType>)
    OLO_FINLINE void MoveAssignItems(ElementType* Dest, const ElementType* Source, SizeType Count)
    {
        FMemory::Memmove(Dest, Source, sizeof(ElementType) * Count);
    }

    /**
     * @brief Move assigns a range of items (non-trivially copy assignable)
     * 
     * @param Dest      The memory location to start move assigning to
     * @param Source    A pointer to the first item to move assign
     * @param Count     The number of elements to move assign
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && !std::is_trivially_copy_assignable_v<ElementType>)
    OLO_NOINLINE void MoveAssignItems(ElementType* Dest, const ElementType* Source, SizeType Count)
    {
        while (Count)
        {
            *Dest = static_cast<ElementType&&>(*const_cast<ElementType*>(Source));
            ++Dest;
            ++Source;
            --Count;
        }
    }

    // ============================================================================
    // Comparison
    // ============================================================================

    /**
     * @brief Compares two ranges of items for equality (bytewise comparable)
     * 
     * @param A         Pointer to first range
     * @param B         Pointer to second range  
     * @param Count     The number of elements to compare
     * @return true if all elements are equal
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && TTypeTraits<ElementType>::IsBytewiseComparable)
    OLO_FINLINE bool CompareItems(const ElementType* A, const ElementType* B, SizeType Count)
    {
        return !Count || !FMemory::Memcmp(A, B, sizeof(ElementType) * Count);
    }

    /**
     * @brief Compares two ranges of items for equality (non-bytewise comparable)
     * 
     * @param A         Pointer to first range
     * @param B         Pointer to second range  
     * @param Count     The number of elements to compare
     * @return true if all elements are equal
     */
    template <typename ElementType, typename SizeType>
        requires (sizeof(ElementType) > 0 && !TTypeTraits<ElementType>::IsBytewiseComparable)
    OLO_NOINLINE bool CompareItems(const ElementType* A, const ElementType* B, SizeType Count)
    {
        while (Count)
        {
            if (!(*A == *B))
            {
                return false;
            }

            ++A;
            ++B;
            --Count;
        }

        return true;
    }

} // namespace OloEngine
