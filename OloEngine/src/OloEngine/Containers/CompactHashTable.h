#pragma once

/**
 * @file CompactHashTable.h
 * @brief Compact hash table implementation for TCompactSet
 *
 * Compact hash table has two distinct features to keep it small:
 * 1. The Index type adapts to the number of elements, so a smaller table will use a smaller type
 * 2. Holes in the table are patched up so all slots up to count are guaranteed to be valid
 *
 * For performance reasons we only reset the HashTable part of the lookup table since the indexes will be unused until added.
 * It's the users responsibility to maintain correct item count
 * !!Important!! It's also the users responsibility to move items from the end of the list into their new spots when removing items.
 *
 * You will be able to maintain different views (hashes) of the same data since hole patching is deterministic between two tables.
 * This can be used to allow searching against the same data using different fields as names
 *
 * Ported from Unreal Engine's Containers/CompactHashTable.h
 */

#include "OloEngine/Containers/ArrayView.h"

namespace OloEngine::CompactHashTable::Private
{
    // Remove an element from the list and patch up any next index references, after this the spot is empty and needs to be filled
    template<typename IndexType>
    inline void RemoveInternal(u32 Index, u32 Key, IndexType* HashData, u32 HashCount, IndexType* NextIndexData, u32 NextIndexCount)
    {
        const u32 HashIndex = Key & (HashCount - 1);

        for (IndexType* Lookup = &HashData[HashIndex]; *Lookup < NextIndexCount; Lookup = &NextIndexData[*Lookup])
        {
            if (*Lookup == Index)
            {
                // Next = Next->Next
                *Lookup = NextIndexData[Index];
                return;
            }
        }
    }
} // namespace OloEngine::CompactHashTable::Private

/* Helper to write code against specific types resolves by number of elements. Usage:
    #define OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(Type) // Code
    OLO_COMPACTHASHTABLE_CALLBYTYPE(NumElements)
    #undef OLO_COMPACTHASHTABLE_EXECUTEBYTYPE
*/
#define OLO_COMPACTHASHTABLE_CALLBYTYPE(NextIndexCount)               \
    switch (OloEngine::CompactHashTable::GetTypeSize(NextIndexCount)) \
    {                                                                 \
        case 1:                                                       \
        {                                                             \
            OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(u8);                   \
        }                                                             \
        break;                                                        \
        case 2:                                                       \
        {                                                             \
            OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(u16);                  \
        }                                                             \
        break;                                                        \
        case 4:                                                       \
        {                                                             \
            OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(u32);                  \
        }                                                             \
        break;                                                        \
        default:                                                      \
            OLO_CORE_ASSERT(false, "Invalid type size");              \
    }

/* Helpers for interacting with compact hash table memory layouts */
namespace OloEngine::CompactHashTable
{
    [[nodiscard]] OLO_FINLINE constexpr u32 GetTypeSize(u32 IndexCount)
    {
        return 1 + static_cast<i32>(IndexCount > 0xff) + static_cast<i32>(IndexCount > 0xffff) * 2;
    }

    [[nodiscard]] OLO_FINLINE constexpr u32 GetTypeShift(u32 IndexCount)
    {
        return 0 + static_cast<i32>(IndexCount > 0xff) + static_cast<i32>(IndexCount > 0xffff);
    }

    [[nodiscard]] OLO_FINLINE constexpr sizet GetMemoryRequiredInBytes(u32 IndexCount, u32 HashCount)
    {
        return (static_cast<sizet>(IndexCount) + HashCount) << GetTypeShift(IndexCount);
    }

    [[nodiscard]] OLO_FINLINE constexpr sizet GetMemoryAlignment()
    {
        return 4; // only support uint32 for now, pick highest alignment so we don't change alignment between allocations
    }

    /* Calculate the size of the hash table from the number of elements in the set */
    [[nodiscard]] inline constexpr sizet GetHashCount(u32 NumElements)
    {
        if (!NumElements)
        {
            return 0;
        }
        if (NumElements < 8)
        {
            return 4;
        }

        // Always use the power of 2 smaller than the current size to prioritize size over speed just a little bit
        // Manual RoundUpToPowerOfTwo implementation since we don't have FGenericPlatformMath
        u32 Value = NumElements / 2 + 1; // 255 == 128, 256 == 256
        --Value;
        Value |= Value >> 1;
        Value |= Value >> 2;
        Value |= Value >> 4;
        Value |= Value >> 8;
        Value |= Value >> 16;
        ++Value;
        return Value;
    }

    /* Return the first index of a key from the hash table portion */
    template<typename IndexType>
    [[nodiscard]] inline u32 GetFirst(u32 Key, const IndexType* HashData, u32 HashCount)
    {
        const u32 HashIndex = Key & (HashCount - 1);
        const IndexType FirstIndex = HashData[HashIndex];

        // Convert smaller type INDEX_NONE to uint32 INDEX_NONE if data is invalid
        constexpr IndexType InvalidIndex = static_cast<IndexType>(INDEX_NONE);
        return FirstIndex == InvalidIndex ? static_cast<u32>(INDEX_NONE) : static_cast<u32>(FirstIndex);
    }

    /* Return the first index of a key from the hash table portion */
    template<typename IndexType>
    [[nodiscard]] inline u32 GetFirstByIndex(u32 HashIndex, const IndexType* HashData, u32 HashCount)
    {
        OLO_CORE_ASSERT(HashIndex <= HashCount, "HashIndex out of bounds");
        const IndexType FirstIndex = HashData[HashIndex];

        // Convert smaller type INDEX_NONE to uint32 INDEX_NONE if data is invalid
        constexpr IndexType InvalidIndex = static_cast<IndexType>(INDEX_NONE);
        return FirstIndex == InvalidIndex ? static_cast<u32>(INDEX_NONE) : static_cast<u32>(FirstIndex);
    }

    /* Given an existing index, return the next index in case there was a collision in the hash table */
    template<typename IndexType>
    [[nodiscard]] inline u32 GetNext(u32 Index, const IndexType* NextIndexData, u32 NextIndexCount)
    {
        OLO_CORE_ASSERT(Index < NextIndexCount, "Index out of bounds");
        const IndexType NextIndex = NextIndexData[Index];

        // Convert smaller type INDEX_NONE to uint32 INDEX_NONE if data is invalid
        constexpr IndexType InvalidIndex = static_cast<IndexType>(INDEX_NONE);
        return NextIndex == InvalidIndex ? static_cast<u32>(INDEX_NONE) : static_cast<u32>(NextIndex);
    }

    /* Do a full search for an existing element in the table given a function to compare if a found element is what you're looking for */
    template<typename IndexType, typename PredicateType>
    [[nodiscard]] inline u32 Find(u32 Key, const IndexType* HashData, u32 HashCount, const IndexType* NextIndexData, u32 NextIndexCount, const PredicateType& Predicate)
    {
        for (IndexType ElementIndex = HashData[Key & (HashCount - 1)]; ElementIndex != static_cast<IndexType>(INDEX_NONE); ElementIndex = NextIndexData[ElementIndex])
        {
            if (Predicate(ElementIndex))
            {
                // Return the first match, regardless of whether the set has multiple matches for the key or not.
                return ElementIndex;
            }
            OLO_CORE_ASSERT(ElementIndex < NextIndexCount, "Index chain corrupt");
        }
        return INDEX_NONE;
    }

    /* Insert new element into the hash table */
    template<typename IndexType>
    inline void Add(u32 Index, u32 Key, IndexType* HashData, u32 HashCount, IndexType* NextIndexData, u32 NextIndexCount)
    {
        OLO_CORE_ASSERT(Index < NextIndexCount, "Index out of bounds");

        const u32 HashIndex = Key & (HashCount - 1);
        NextIndexData[Index] = HashData[HashIndex];
        HashData[HashIndex] = static_cast<IndexType>(Index);
    }

    /* Remove an element from the list, move the last element into the now empty slot
       If the item to remove is the last element then the last element's key will be ignored (you can skip calculating it if it's expensive)
    */
    template<typename IndexType>
    inline void Remove(u32 Index, u32 Key, u32 LastIndex, u32 OptLastKey, IndexType* HashData, u32 HashCount, IndexType* NextIndexData, u32 NextIndexCount)
    {
        OLO_CORE_ASSERT(LastIndex < NextIndexCount && Index <= LastIndex, "Invalid indices");

        OloEngine::CompactHashTable::Private::RemoveInternal<IndexType>(Index, Key, HashData, HashCount, NextIndexData, NextIndexCount);

        if (Index != LastIndex)
        {
            // Remove the last element and add it into the empty spot
            OloEngine::CompactHashTable::Private::RemoveInternal<IndexType>(LastIndex, OptLastKey, HashData, HashCount, NextIndexData, NextIndexCount);

            const u32 HashIndex = OptLastKey & (HashCount - 1);
            NextIndexData[Index] = HashData[HashIndex];
            HashData[HashIndex] = static_cast<IndexType>(Index);
        }
    }

    /* Remove an element from the list, shift all indexes down to preserve the order of elements in a list
       This is a very expensive operation so it should only be used if absolutely necessary (i.e. generally only for user facing data)
    */
    template<typename IndexType>
    inline void RemoveStable(u32 Index, u32 Key, IndexType* HashData, u32 HashCount, IndexType* NextIndexData, u32 NextIndexCount)
    {
        OLO_CORE_ASSERT(Index < NextIndexCount, "Index out of bounds");

        OloEngine::CompactHashTable::Private::RemoveInternal<IndexType>(Index, Key, HashData, HashCount, NextIndexData, NextIndexCount);

        // For the hash indexes, just decrement any that are bigger than the removed element
        for (u32 HashIndex = 0; HashIndex < HashCount; ++HashIndex)
        {
            if (HashData[HashIndex] > Index && HashData[HashIndex] != static_cast<IndexType>(INDEX_NONE))
            {
                --HashData[HashIndex];
            }
        }

        // Decrement values for all next index elements that are before the removed element
        for (u32 NextIndexIndex = 0; NextIndexIndex < Index; ++NextIndexIndex)
        {
            if (NextIndexData[NextIndexIndex] > Index && NextIndexData[NextIndexIndex] != static_cast<IndexType>(INDEX_NONE))
            {
                --NextIndexData[NextIndexIndex];
            }
        }

        // Move AND Decrement values for all next index elements that are after the removed element
        for (u32 NextIndexIndex = Index + 1; NextIndexIndex < NextIndexCount; ++NextIndexIndex)
        {
            if (NextIndexData[NextIndexIndex] > Index && NextIndexData[NextIndexIndex] != static_cast<IndexType>(INDEX_NONE))
            {
                NextIndexData[NextIndexIndex - 1] = NextIndexData[NextIndexIndex] - 1;
            }
            else
            {
                NextIndexData[NextIndexIndex - 1] = NextIndexData[NextIndexIndex];
            }
        }
    }
} // namespace OloEngine::CompactHashTable

namespace OloEngine
{

    /* Helper to lookup a type from a type size, todo: Add support for 3 byte lookup */
    template<u32 TypeSize>
    struct TCompactHashTypeLookupBySize;

    template<>
    struct TCompactHashTypeLookupBySize<1>
    {
        using Type = u8;
    };

    template<>
    struct TCompactHashTypeLookupBySize<2>
    {
        using Type = u16;
    };

    template<>
    struct TCompactHashTypeLookupBySize<4>
    {
        using Type = u32;
    };

    /* Helper for making a fixed size hash table that manages its own memory */
    template<u32 ElementCount, u32 HashCount = CompactHashTable::GetHashCount(ElementCount)>
    class TStaticCompactHashTable
    {
      public:
        using IndexType = typename TCompactHashTypeLookupBySize<CompactHashTable::GetTypeSize(ElementCount)>::Type;

        TStaticCompactHashTable()
        {
            static_assert(FMath::IsPowerOfTwo(HashCount));
            Reset();
        }
        TStaticCompactHashTable(ENoInit) {}

        OLO_FINLINE void Reset()
        {
            FMemory::Memset(HashData, 0xff, sizeof(HashData));
        }

        [[nodiscard]] OLO_FINLINE u32 GetFirst(u32 Key) const
        {
            return CompactHashTable::GetFirst(Key, HashData, HashCount);
        }

        [[nodiscard]] OLO_FINLINE u32 GetFirstByIndex(u32 HashIndex) const
        {
            return CompactHashTable::GetFirstByIndex(HashIndex, HashData, HashCount);
        }

        [[nodiscard]] inline u32 GetNext(u32 Index, u32 CurrentCount) const
        {
            OLO_CORE_ASSERT(CurrentCount <= ElementCount, "CurrentCount exceeds ElementCount");
            return CompactHashTable::GetNext(Index, NextIndexData, CurrentCount);
        }

        template<typename PredicateType>
        [[nodiscard]] inline u32 Find(u32 Key, u32 CurrentCount, const PredicateType& Predicate) const
        {
            OLO_CORE_ASSERT(CurrentCount <= ElementCount, "CurrentCount exceeds ElementCount");
            return CompactHashTable::Find(Key, HashData, HashCount, NextIndexData, CurrentCount, Predicate);
        }

        inline void Add(u32 CurrentCount, u32 Key)
        {
            OLO_CORE_ASSERT(CurrentCount < ElementCount, "Cannot add: ElementCount exceeded");
            CompactHashTable::Add(CurrentCount, Key, HashData, HashCount, NextIndexData, CurrentCount + 1);
        }

        OLO_FINLINE void Remove(u32 Index, u32 Key, u32 LastIndex, u32 OptLastKey)
        {
            CompactHashTable::Remove(Index, Key, LastIndex, OptLastKey, HashData, HashCount, NextIndexData, ElementCount);
        }

      protected:
        IndexType NextIndexData[ElementCount]; // Collision redirector to next index for keys that hash to the same initial index
        IndexType HashData[HashCount];         // First index lookup from key
    };

    /* Helper for interacting with existing hash table memory */
    class FConstCompactHashTableView
    {
      public:
        explicit FConstCompactHashTableView() = default;

        inline explicit FConstCompactHashTableView(const u8* Memory, u32 InNextIndexCount, u32 InHashCount, sizet MemorySize)
            : NextIndexData(Memory), HashData(Memory + (static_cast<sizet>(InNextIndexCount) << CompactHashTable::GetTypeShift(InNextIndexCount))), NextIndexCount(InNextIndexCount), HashCount(InHashCount)
        {
            OLO_CORE_ASSERT(Memory != nullptr && InNextIndexCount > 0 && InHashCount > 0 && MemorySize > 0, "Invalid hash table view parameters");
            OLO_CORE_ASSERT(MemorySize == CompactHashTable::GetMemoryRequiredInBytes(NextIndexCount, HashCount), "Memory size mismatch");
            OLO_CORE_ASSERT(FMath::IsPowerOfTwo(HashCount), "HashCount must be power of two");
        }

        [[nodiscard]] u32 GetHashCount() const
        {
            return HashCount;
        }

        // Functions used to search
        [[nodiscard]] inline u32 GetFirst(u32 Key) const
        {
#define OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(Type) return CompactHashTable::GetFirst(Key, reinterpret_cast<const Type*>(HashData), HashCount)
            OLO_COMPACTHASHTABLE_CALLBYTYPE(NextIndexCount);
#undef OLO_COMPACTHASHTABLE_EXECUTEBYTYPE
            return INDEX_NONE;
        }

        // Advanced used for manual inspection of the hash data
        [[nodiscard]] inline u32 GetFirstByIndex(u32 HashIndex) const
        {
#define OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(Type) return CompactHashTable::GetFirstByIndex(HashIndex, reinterpret_cast<const Type*>(HashData), HashCount)
            OLO_COMPACTHASHTABLE_CALLBYTYPE(NextIndexCount);
#undef OLO_COMPACTHASHTABLE_EXECUTEBYTYPE
            return INDEX_NONE;
        }

        [[nodiscard]] inline u32 GetNext(u32 Index, u32 CurrentCount) const
        {
            OLO_CORE_ASSERT(CurrentCount <= NextIndexCount, "CurrentCount exceeds NextIndexCount");

#define OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(Type) return CompactHashTable::GetNext(Index, reinterpret_cast<const Type*>(NextIndexData), CurrentCount)
            OLO_COMPACTHASHTABLE_CALLBYTYPE(NextIndexCount);
#undef OLO_COMPACTHASHTABLE_EXECUTEBYTYPE
            return INDEX_NONE;
        }

        template<typename PredicateType>
        [[nodiscard]] inline u32 Find(u32 Key, u32 CurrentCount, const PredicateType& Predicate) const
        {
            OLO_CORE_ASSERT(CurrentCount <= NextIndexCount, "CurrentCount exceeds NextIndexCount");
#define OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(Type) return CompactHashTable::Find(Key, reinterpret_cast<const Type*>(HashData), HashCount, reinterpret_cast<const Type*>(NextIndexData), CurrentCount, Predicate)
            OLO_COMPACTHASHTABLE_CALLBYTYPE(NextIndexCount);
#undef OLO_COMPACTHASHTABLE_EXECUTEBYTYPE
            return INDEX_NONE;
        }

      protected:
        const u8* NextIndexData = nullptr;
        const u8* HashData = nullptr;
        u32 NextIndexCount = 0;
        u32 HashCount = 0;
    };

    /* Helper for interacting with existing hash table memory */
    class FCompactHashTableView : public FConstCompactHashTableView
    {
      public:
        OLO_FINLINE explicit FCompactHashTableView(u8* Memory, u32 InNextIndexCount, u32 InHashCount, sizet MemorySize)
            : FConstCompactHashTableView(Memory, InNextIndexCount, InHashCount, MemorySize)
        {
        }

        inline void Reset() const
        {
            const u32 TypeShift = CompactHashTable::GetTypeShift(NextIndexCount);

            // The const_casts are a little gross but I don't want to maintain duplicate copies of the const functions
            FMemory::Memset(const_cast<u8*>(HashData), 0xff, static_cast<sizet>(HashCount) << TypeShift);
        }

        inline void Add(u32 Index, u32 Key) const
        {
            OLO_CORE_ASSERT(Index < NextIndexCount, "Index out of bounds");
#define OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(Type) CompactHashTable::Add(Index, Key, reinterpret_cast<Type*>(const_cast<u8*>(HashData)), HashCount, reinterpret_cast<Type*>(const_cast<u8*>(NextIndexData)), NextIndexCount)
            OLO_COMPACTHASHTABLE_CALLBYTYPE(NextIndexCount);
#undef OLO_COMPACTHASHTABLE_EXECUTEBYTYPE
        }

        OLO_FINLINE void Remove(u32 Index, u32 Key, u32 LastIndex, u32 OptLastKey) const
        {
#define OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(Type) return CompactHashTable::Remove(Index, Key, LastIndex, OptLastKey, reinterpret_cast<Type*>(const_cast<u8*>(HashData)), HashCount, reinterpret_cast<Type*>(const_cast<u8*>(NextIndexData)), NextIndexCount)
            OLO_COMPACTHASHTABLE_CALLBYTYPE(NextIndexCount);
#undef OLO_COMPACTHASHTABLE_EXECUTEBYTYPE
        }

        OLO_FINLINE void RemoveStable(u32 Index, u32 Key) const
        {
#define OLO_COMPACTHASHTABLE_EXECUTEBYTYPE(Type) return CompactHashTable::RemoveStable(Index, Key, reinterpret_cast<Type*>(const_cast<u8*>(HashData)), HashCount, reinterpret_cast<Type*>(const_cast<u8*>(NextIndexData)), NextIndexCount)
            OLO_COMPACTHASHTABLE_CALLBYTYPE(NextIndexCount);
#undef OLO_COMPACTHASHTABLE_EXECUTEBYTYPE
        }
    };

} // namespace OloEngine
