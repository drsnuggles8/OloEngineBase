#pragma once

/**
 * @file SparseSetElement.h
 * @brief Element wrapper for TSparseSet containers
 * 
 * Provides:
 * - TSparseSetElement: Wrapper that adds hash chain linking to elements
 * - Helper functions for hash table operations
 * 
 * Ported from Unreal Engine's Containers/SparseSetElement.h
 */

#include "OloEngine/Containers/SetUtilities.h"
#include "OloEngine/Serialization/Archive.h"
#include <initializer_list>
#include <type_traits>

namespace OloEngine
{
    /**
     * @class TSparseSetElement
     * @brief An element in the set that stores value and hash chain linking
     * 
     * Stores the element value plus hash bucket linking information.
     */
    template <typename InElementType>
    class TSparseSetElement
    {
    public:
        using ElementType = InElementType;

        /** Default constructor */
       TSparseSetElement() = default;

        /** Initialization constructor */
        template <
            typename... InitType,
            typename = std::enable_if_t<
                (sizeof...(InitType) != 1) || 
                (!std::is_same_v<TSparseSetElement, std::decay_t<InitType>> && ...)
            >
        >
        [[nodiscard]] explicit OLO_FINLINE TSparseSetElement(InitType&&... InValue)
            : Value(Forward<InitType>(InValue)...)
        {
        }

        TSparseSetElement(TSparseSetElement&&) = default;
        TSparseSetElement(const TSparseSetElement&) = default;
        TSparseSetElement& operator=(TSparseSetElement&&) = default;
        TSparseSetElement& operator=(const TSparseSetElement&) = default;

        /** Comparison operators */
        [[nodiscard]] OLO_FINLINE bool operator==(const TSparseSetElement& Other) const
        {
            return Value == Other.Value;
        }

        [[nodiscard]] OLO_FINLINE bool operator!=(const TSparseSetElement& Other) const
        {
            return Value != Other.Value;
        }

        /** The element's value */
        ElementType Value;

        /** The id of the next element in the same hash bucket */
        mutable FSetElementId HashNextId;

        /** The hash bucket that the element is currently linked to */
        mutable i32 HashIndex;
    };

    // ============================================================================
    // Internal Helper Functions (matching UE::Core::Private namespace)
    // ============================================================================

    namespace SparseSetPrivate
    {
        /** Error handler for invalid set operations */
        [[noreturn]] inline void OnInvalidSetNum(u64 NewNum)
        {
            OLO_CORE_ASSERT(false, "Invalid set size: {}", NewNum);
            std::abort();
        }

        /** Copy hash buckets from one allocator to another */
        template <typename HashType>
        void CopyHash(HashType& Hash, i32& HashSize, const HashType& Copy, i32 HashSizeCopy)
        {
            DestructItems(reinterpret_cast<FSetElementId*>(Hash.GetAllocation()), HashSize);
            Hash.ResizeAllocation(0, HashSizeCopy, sizeof(FSetElementId));
            ConstructItems<FSetElementId>(Hash.GetAllocation(), reinterpret_cast<FSetElementId*>(Copy.GetAllocation()), HashSizeCopy);
            HashSize = HashSizeCopy;
        }

        /** Get hash bucket at index */
        template <typename HashType>
        [[nodiscard]] OLO_FINLINE FSetElementId& GetTypedHash(HashType& Hash, i32 HashIndex, i32 HashSize)
        {
            return reinterpret_cast<FSetElementId*>(Hash.GetAllocation())[HashIndex & (HashSize - 1)];
        }

        /** Get hash bucket at index (const) */
        template <typename HashType>
        [[nodiscard]] OLO_FINLINE const FSetElementId& GetTypedHash(const HashType& Hash, i32 HashIndex, i32 HashSize)
        {
            return reinterpret_cast<const FSetElementId*>(Hash.GetAllocation())[HashIndex & (HashSize - 1)];
        }

        /** Reallocate and reinitialize hash table */
        template <typename HashType>
        void Rehash(HashType& Hash, i32 HashSize)
        {
            // Free the old hash
            Hash.ResizeAllocation(0, 0, sizeof(FSetElementId));

            if (HashSize)
            {
                // Allocate the new hash (must be power of two)
                OLO_CORE_ASSERT(FMath::IsPowerOfTwo(HashSize), "HashSize must be power of two");
                Hash.ResizeAllocation(0, HashSize, sizeof(FSetElementId));
                for (i32 HashIdx = 0; HashIdx < HashSize; ++HashIdx)
                {
                    GetTypedHash(Hash, HashIdx, HashSize) = FSetElementId();
                }
            }
        }
    } // namespace SparseSetPrivate

    /** Serializer */
    template <typename ElementType>
    OLO_FINLINE FArchive& operator<<(FArchive& Ar, TSparseSetElement<ElementType>& Element)
    {
        return Ar << Element.Value;
    }

} // namespace OloEngine
