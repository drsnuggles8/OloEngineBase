#pragma once

/**
 * @file SparseSet.h
 * @brief Hash-based set container with O(1) average operations
 *
 * Provides a hash-based set implementation using TSparseArray for element storage:
 * - O(1) average case for add, remove, and find operations
 * - Customizable key functions for different comparison and hashing strategies
 * - Support for heterogeneous lookup with ByHash() functions
 * - Iteration maintains insertion order (via sparse array)
 *
 * Ported from Unreal Engine's Containers/SparseSet.h.inl
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/SparseArray.h"
#include "OloEngine/Containers/SparseSetElement.h"
#include "OloEngine/Containers/SetUtilities.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Containers/Array.h" // For EAllowShrinking
#include "OloEngine/Serialization/Archive.h"
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace OloEngine
{
    // Forward declarations
    template<typename KeyType, typename ValueType>
    struct TPair;

    /**
     * @class TSparseSet
     * @brief A hash-based set with optional custom key functions
     *
     * Uses a TSparseArray to store elements and a hash table for O(1) lookup.
     * Elements are linked in hash chains for collision resolution.
     *
     * @tparam InElementType  The element type stored in the set
     * @tparam KeyFuncs       Functions for getting keys and computing hashes
     * @tparam Allocator      Allocator policy for elements and hash table
     */
    template<
        typename InElementType,
        typename KeyFuncs = DefaultKeyFuncs<InElementType>,
        typename Allocator = FDefaultSetAllocator>
    class TSparseSet
    {
        friend struct TSparseSetPrivateFriend;

      public:
        using ElementType = InElementType;
        using KeyFuncsType = KeyFuncs;
        using AllocatorType = Allocator;
        using SizeType = i32;

      private:
        using KeyInitType = typename KeyFuncs::KeyInitType;
        using ElementInitType = typename KeyFuncs::ElementInitType;
        using SetElementType = TSparseSetElement<InElementType>;

        // The sparse array of elements
        using ElementArrayType = TSparseArray<SetElementType, typename Allocator::SparseArrayAllocator>;
        ElementArrayType Elements;

        // The hash table - maps hash values to element indices
        using HashType = typename Allocator::HashAllocator::template ForElementType<FSetElementId>;
        mutable HashType Hash;
        i32 HashSize = 0;

      public:
        // ========================================================================
        // Intrusive TOptional<TSparseSet> State
        // ========================================================================

        /** Enables intrusive optional state for this type */
        constexpr static bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TSparseSet;

        /** Constructor for intrusive optional unset state */
        [[nodiscard]] explicit TSparseSet(FIntrusiveUnsetOptionalState Tag)
            : Elements(Tag)
        {
        }

        /** Comparison with intrusive optional unset state */
        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState Tag) const
        {
            return Elements == Tag;
        }

        // ========================================================================
        // Constructors / Destructor
        // ========================================================================

        /** Default constructor */
        [[nodiscard]] OLO_FINLINE constexpr TSparseSet() = default;

        /** Explicitly consteval constructor for compile-time constant sets */
        [[nodiscard]] explicit consteval TSparseSet(EConstEval)
            : Elements(ConstEval)
        {
        }

        /** Copy constructor */
        [[nodiscard]] OLO_FINLINE TSparseSet(const TSparseSet& Copy)
        {
            *this = Copy;
        }

        /** Move constructor */
        [[nodiscard]] TSparseSet(TSparseSet&& Other)
            : HashSize(0)
        {
            Move(*this, Other);
        }

        /** Initializer list constructor */
        [[nodiscard]] TSparseSet(std::initializer_list<ElementType> InitList)
            : HashSize(0)
        {
            Append(InitList);
        }

        /** Destructor */
        OLO_FINLINE ~TSparseSet()
        {
            HashSize = 0;
        }

        // ========================================================================
        // Assignment Operators
        // ========================================================================

        /** Copy assignment */
        TSparseSet& operator=(const TSparseSet& Copy)
        {
            if (this != &Copy)
            {
                SparseSetPrivate::CopyHash(Hash, HashSize, Copy.Hash, Copy.HashSize);
                Elements = Copy.Elements;
            }
            return *this;
        }

        /** Move assignment */
        TSparseSet& operator=(TSparseSet&& Other)
        {
            if (this != &Other)
            {
                Move(*this, Other);
            }
            return *this;
        }

        /** Initializer list assignment */
        TSparseSet& operator=(std::initializer_list<ElementType> InitList)
        {
            Reset();
            Append(InitList);
            return *this;
        }

      private:
        template<typename SetType>
        static inline void Move(SetType& ToSet, SetType& FromSet)
        {
            ToSet.Elements = MoveTemp(FromSet.Elements);
            ToSet.Hash.MoveToEmpty(FromSet.Hash);
            ToSet.HashSize = FromSet.HashSize;
            FromSet.HashSize = 0;
        }

      public:
        // ========================================================================
        // Size / Capacity
        // ========================================================================

        /** @return true if the set is empty */
        [[nodiscard]] bool IsEmpty() const
        {
            return Elements.IsEmpty();
        }

        /** @return the number of elements */
        [[nodiscard]] OLO_FINLINE i32 Num() const
        {
            return Elements.Num();
        }

        /** @return the number of elements the set can hold before reallocation */
        [[nodiscard]] OLO_FINLINE i32 Max() const
        {
            return Elements.Max();
        }

        /** @return the non-inclusive maximum index of elements in the set */
        [[nodiscard]] OLO_FINLINE i32 GetMaxIndex() const
        {
            return Elements.GetMaxIndex();
        }

        /** @return the amount of memory allocated by this container */
        [[nodiscard]] OLO_FINLINE sizet GetAllocatedSize() const
        {
            return Elements.GetAllocatedSize() + static_cast<sizet>(HashSize) * sizeof(FSetElementId);
        }

        /** Tracks the container's memory use through an archive */
        void CountBytes(FArchive& Ar) const
        {
            Elements.CountBytes(Ar);
            Ar.CountBytes(HashSize * sizeof(i32), HashSize * sizeof(FSetElementId));
        }

        // ========================================================================
        // Element Access
        // ========================================================================

        /**
         * Checks whether an element id is valid.
         * @param Id  The element id to check
         * @return true if the id refers to a valid element
         */
        [[nodiscard]] inline bool IsValidId(FSetElementId Id) const
        {
            i32 Index = Id.AsInteger();
            return Index != INDEX_NONE &&
                   Index >= 0 &&
                   Index < Elements.GetMaxIndex() &&
                   Elements.IsAllocated(Index);
        }

        /** Access element by id */
        [[nodiscard]] OLO_FINLINE ElementType& operator[](FSetElementId Id)
        {
            return Elements[Id.AsInteger()].Value;
        }

        /** Access element by id (const) */
        [[nodiscard]] OLO_FINLINE const ElementType& operator[](FSetElementId Id) const
        {
            return Elements[Id.AsInteger()].Value;
        }

        /** Access the identified element's value. Element must be valid (see @IsValidId). */
        [[nodiscard]] OLO_FINLINE ElementType& Get(FSetElementId Id)
        {
            return Elements[Id.AsInteger()].Value;
        }

        /** Access the identified element's value. Element must be valid (see @IsValidId). */
        [[nodiscard]] OLO_FINLINE const ElementType& Get(FSetElementId Id) const
        {
            return Elements[Id.AsInteger()].Value;
        }

        // ========================================================================
        // Modification
        // ========================================================================

        /**
         * @brief Remove all elements
         * @param ExpectedNumElements  Number of elements expected to be added
         */
        void Empty(i32 ExpectedNumElements = 0)
        {
            const i32 DesiredHashSize = Allocator::GetNumberOfHashBuckets(ExpectedNumElements);
            const bool ShouldDoRehash = ShouldRehash(ExpectedNumElements, DesiredHashSize, true);

            if (!ShouldDoRehash)
            {
                UnhashElements();
            }

            Elements.Empty(ExpectedNumElements);

            if (ShouldDoRehash)
            {
                HashSize = DesiredHashSize;
                Rehash();
            }
        }

        /** Empty the set but keep allocations */
        void Reset()
        {
            if (Num() == 0)
            {
                return;
            }

            UnhashElements();
            Elements.Reset();
        }

        /** Shrink element storage to avoid slack */
        inline void Shrink()
        {
            Elements.Shrink();
            Relax();
        }

        /** Compact elements into a contiguous range */
        inline void Compact()
        {
            if (Elements.Compact())
            {
                HashSize = Allocator::GetNumberOfHashBuckets(Elements.Num());
                Rehash();
            }
        }

        /** Compact elements while preserving iteration order */
        inline void CompactStable()
        {
            if (Elements.CompactStable())
            {
                HashSize = Allocator::GetNumberOfHashBuckets(Elements.Num());
                Rehash();
            }
        }

        /**
         * @brief Sort the set's elements using the provided comparison class
         * @param Predicate  Comparison functor
         */
        template<typename PredicateClass>
        void Sort(const PredicateClass& Predicate)
        {
            // Sort the elements according to the provided comparison class
            Elements.Sort(FElementCompareClass<PredicateClass>(Predicate));

            // Rehash after sorting
            Rehash();
        }

        /** Sort elements using operator< */
        void Sort()
        {
            Sort(TLess<ElementType>());
        }

        /**
         * @brief Stable sort the set's elements using the provided comparison class
         * @param Predicate  Comparison functor
         */
        template<typename PredicateClass>
        void StableSort(const PredicateClass& Predicate)
        {
            // Sort the elements according to the provided comparison class
            Elements.StableSort(FElementCompareClass<PredicateClass>(Predicate));

            // Rehash after sorting
            Rehash();
        }

        /** Stable sort elements using operator< */
        void StableSort()
        {
            StableSort(TLess<ElementType>());
        }

        /**
         * @brief Sort the free element list for deterministic allocation order
         * @see TSparseArray::SortFreeList()
         */
        void SortFreeList()
        {
            Elements.SortFreeList();
        }

        /** Preallocate memory for Number elements */
        inline void Reserve(i32 Number)
        {
            if (Number > Elements.Num())
            {
                Elements.Reserve(Number);

                const i32 NewHashSize = Allocator::GetNumberOfHashBuckets(Number);

                if (!HashSize || HashSize < NewHashSize)
                {
                    HashSize = NewHashSize;
                    Rehash();
                }
            }
        }

        /** Relax hash to match element count */
        OLO_FINLINE void Relax()
        {
            ConditionalRehash(Elements.Num(), EAllowShrinking::Yes);
        }

        // ========================================================================
        // Add / Emplace
        // ========================================================================

        /**
         * @brief Add an element to the set
         * @param InElement  Element to add
         * @param bIsAlreadyInSetPtr  Optional out parameter set if element was already in set
         * @return The id of the element in the set
         */
        OLO_FINLINE FSetElementId Add(const InElementType& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return Emplace(InElement, bIsAlreadyInSetPtr);
        }

        OLO_FINLINE FSetElementId Add(InElementType&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return Emplace(MoveTemp(InElement), bIsAlreadyInSetPtr);
        }

        /**
         * @brief Add element if not present, return reference to existing or new element
         * @param InElement  Element to add
         * @param bIsAlreadyInSetPtr  Optional out parameter set if element was already in set
         * @return Reference to the element in the set
         */
        OLO_FINLINE ElementType& FindOrAdd(const InElementType& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return FindOrAddByHash(KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(InElement)), InElement, bIsAlreadyInSetPtr);
        }

        OLO_FINLINE ElementType& FindOrAdd(InElementType&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return FindOrAddByHash(KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(InElement)), MoveTemp(InElement), bIsAlreadyInSetPtr);
        }

        /**
         * @brief Add element with precomputed hash
         * @param KeyHash  Precomputed hash value
         * @param InElement  Element to add
         * @param bIsAlreadyInSetPtr  Optional out parameter
         * @return The id of the element in the set
         */
        OLO_FINLINE FSetElementId AddByHash(u32 KeyHash, const InElementType& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return EmplaceByHash(KeyHash, InElement, bIsAlreadyInSetPtr);
        }

        OLO_FINLINE FSetElementId AddByHash(u32 KeyHash, InElementType&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return EmplaceByHash(KeyHash, MoveTemp(InElement), bIsAlreadyInSetPtr);
        }

        /**
         * @brief Add element if not present (with precomputed hash)
         * @param KeyHash  Precomputed hash value
         * @param InElement  Element to add
         * @param bIsAlreadyInSetPtr  Optional out parameter
         * @return Reference to the element in the set
         */
        template<typename ElementReferenceType>
        ElementType& FindOrAddByHash(u32 KeyHash, ElementReferenceType&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            i32 ExistingIndex = FindIndexByHash(KeyHash, KeyFuncs::GetSetKey(InElement));
            bool bIsAlreadyInSet = ExistingIndex != INDEX_NONE;
            if (bIsAlreadyInSetPtr)
            {
                *bIsAlreadyInSetPtr = bIsAlreadyInSet;
            }
            if (bIsAlreadyInSet)
            {
                return Elements[ExistingIndex].Value;
            }

            // Create a new element
            FSparseArrayAllocationInfo ElementAllocation = Elements.AddUninitialized();
            SetElementType& Element = *new (ElementAllocation) SetElementType(Forward<ElementReferenceType>(InElement));
            RehashOrLink(KeyHash, Element, ElementAllocation.Index);
            return Element.Value;
        }

        /**
         * @brief Construct an element in place
         * @param Arg  Arguments forwarded to element constructor
         * @param bIsAlreadyInSetPtr  Optional out parameter
         * @return The id of the element in the set
         */
        template<typename ArgType = ElementType>
        FSetElementId Emplace(ArgType&& Arg, bool* bIsAlreadyInSetPtr = nullptr)
        {
            // Create a new element
            FSparseArrayAllocationInfo ElementAllocation = Elements.AddUninitialized();
            SetElementType& Element = *new (ElementAllocation) SetElementType(Forward<ArgType>(Arg));

            i32 NewHashIndex = ElementAllocation.Index;

            u32 KeyHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Element.Value));
            if (!TryReplaceExisting(KeyHash, Element, NewHashIndex, bIsAlreadyInSetPtr))
            {
                RehashOrLink(KeyHash, Element, NewHashIndex);
            }
            return FSetElementId::FromInteger(NewHashIndex);
        }

        /**
         * @brief Adds an element to the set by constructing the ElementType in-place with multiple args
         * @param InPlaceTag  Tag to disambiguate in-place construction
         * @param InArgs  Arguments forwarded to ElementType's constructor
         * @return Pair of (element id, whether an equivalent element already existed)
         */
        template<typename... ArgTypes>
        TPair<FSetElementId, bool> Emplace(EInPlace, ArgTypes&&... InArgs)
        {
            // Create a new element
            FSparseArrayAllocationInfo ElementAllocation = Elements.AddUninitialized();
            SetElementType& Element = *new (ElementAllocation) SetElementType(Forward<ArgTypes>(InArgs)...);

            i32 NewHashIndex = ElementAllocation.Index;
            u32 const KeyHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Element.Value));

            bool bAlreadyInSet = false;
            if (!TryReplaceExisting(KeyHash, Element, NewHashIndex, &bAlreadyInSet))
            {
                RehashOrLink(KeyHash, Element, NewHashIndex);
            }

            return TPair<FSetElementId, bool>(FSetElementId::FromInteger(NewHashIndex), bAlreadyInSet);
        }

        /**
         * @brief Construct an element in place with precomputed hash
         * @param KeyHash  Precomputed hash value
         * @param Arg  Arguments forwarded to element constructor
         * @param bIsAlreadyInSetPtr  Optional out parameter
         * @return The id of the element in the set
         */
        template<typename ArgType = ElementType>
        FSetElementId EmplaceByHash(u32 KeyHash, ArgType&& Arg, bool* bIsAlreadyInSetPtr = nullptr)
        {
            FSparseArrayAllocationInfo ElementAllocation = Elements.AddUninitialized();
            SetElementType& Element = *new (ElementAllocation) SetElementType(Forward<ArgType>(Arg));

            i32 NewHashIndex = ElementAllocation.Index;

            if (!TryReplaceExisting(KeyHash, Element, NewHashIndex, bIsAlreadyInSetPtr))
            {
                RehashOrLink(KeyHash, Element, NewHashIndex);
            }
            return FSetElementId::FromInteger(NewHashIndex);
        }

        /**
         * @brief Adds an element to the set by constructing in-place with multiple args, using a precomputed hash
         *
         * @see Class documentation section on ByHash() functions
         * @param InPlaceTag  Tag to disambiguate in-place construction
         * @param KeyHash  A precomputed hash value, calculated in the same way as ElementType is hashed
         * @param InArgs  Arguments forwarded to ElementType's constructor
         * @return Pair of (element id, whether an equivalent element already existed)
         */
        template<typename... ArgTypes>
        TPair<FSetElementId, bool> EmplaceByHash(EInPlace, u32 KeyHash, ArgTypes&&... InArgs)
        {
            // Create a new element
            FSparseArrayAllocationInfo ElementAllocation = Elements.AddUninitialized();
            SetElementType& Element = *new (ElementAllocation) SetElementType(Forward<ArgTypes>(InArgs)...);

            i32 NewHashIndex = ElementAllocation.Index;
            bool bAlreadyInSet = false;
            if (!TryReplaceExisting(KeyHash, Element, NewHashIndex, &bAlreadyInSet))
            {
                RehashOrLink(KeyHash, Element, NewHashIndex);
            }
            return TPair<FSetElementId, bool>(FSetElementId::FromInteger(NewHashIndex), bAlreadyInSet);
        }

        /** Append elements from an initializer list */
        void Append(std::initializer_list<ElementType> InitList)
        {
            Reserve(Elements.Num() + static_cast<i32>(InitList.size()));
            for (const ElementType& Element : InitList)
            {
                Add(Element);
            }
        }

        /** Append elements from another set */
        template<typename OtherAllocator>
        void Append(const TSparseSet<ElementType, KeyFuncs, OtherAllocator>& OtherSet)
        {
            Reserve(Elements.Num() + OtherSet.Num());
            for (const ElementType& Element : OtherSet)
            {
                Add(Element);
            }
        }

        /** Append elements from another set (move) */
        template<typename OtherAllocator>
        void Append(TSparseSet<ElementType, KeyFuncs, OtherAllocator>&& OtherSet)
        {
            Reserve(Elements.Num() + OtherSet.Num());
            for (ElementType& Element : OtherSet)
            {
                Add(MoveTemp(Element));
            }
            OtherSet.Reset();
        }

        // ========================================================================
        // Remove
        // ========================================================================

        /**
         * @brief Remove an element by id
         * @param ElementId  The id of the element to remove
         */
        void Remove(FSetElementId ElementId)
        {
            RemoveByIndex(ElementId.AsInteger());
        }

        /**
         * @brief Remove all elements matching the given key
         * @param Key  The key to match
         * @return Number of elements removed
         */
        i32 Remove(KeyInitType Key)
        {
            if (Elements.Num())
            {
                return RemoveImpl(KeyFuncs::GetKeyHash(Key), Key);
            }
            return 0;
        }

        /**
         * @brief Remove element by key with precomputed hash
         * @param KeyHash  Precomputed hash value
         * @param Key  The key to match
         * @return Number of elements removed
         */
        template<typename ComparableKey>
        i32 RemoveByHash(u32 KeyHash, const ComparableKey& Key)
        {
            if (Elements.Num())
            {
                return RemoveImpl(KeyHash, Key);
            }
            return 0;
        }

        /**
         * @brief Remove all elements matching the given key while maintaining set order
         * @param Key  The key to match
         * @return Number of elements removed
         */
        i32 RemoveStable(KeyInitType Key)
        {
            i32 Result = 0;
            if (Elements.Num())
            {
                Result = RemoveImpl(KeyFuncs::GetKeyHash(Key), Key);
                CompactStable();
            }
            return Result;
        }

        // ========================================================================
        // Find / Contains
        // ========================================================================

        /**
         * @brief Find element id by key
         * @param Key  The key to search for
         * @return Element id or invalid id if not found
         */
        [[nodiscard]] FSetElementId FindId(KeyInitType Key) const
        {
            return FSetElementId::FromInteger(FindIndexByHash(KeyFuncs::GetKeyHash(Key), Key));
        }

        /**
         * @brief Find element id with precomputed hash
         * @param KeyHash  Precomputed hash value
         * @param Key  The key to search for
         * @return Element id or invalid id if not found
         */
        template<typename ComparableKey>
        [[nodiscard]] FSetElementId FindIdByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            return FSetElementId::FromInteger(FindIndexByHash(KeyHash, Key));
        }

        /**
         * @brief Find element by key
         * @param Key  The key to search for
         * @return Pointer to element or nullptr
         */
        [[nodiscard]] inline ElementType* Find(KeyInitType Key)
        {
            i32 ElementIndex = FindIndexByHash(KeyFuncs::GetKeyHash(Key), Key);
            if (ElementIndex != INDEX_NONE)
            {
                return &Elements[ElementIndex].Value;
            }
            return nullptr;
        }

        [[nodiscard]] OLO_FINLINE const ElementType* Find(KeyInitType Key) const
        {
            return const_cast<TSparseSet*>(this)->Find(Key);
        }

        /**
         * @brief Find element with precomputed hash
         * @param KeyHash  Precomputed hash value
         * @param Key  The key to search for
         * @return Pointer to element or nullptr
         */
        template<typename ComparableKey>
        [[nodiscard]] ElementType* FindByHash(u32 KeyHash, const ComparableKey& Key)
        {
            i32 ElementIndex = FindIndexByHash(KeyHash, Key);
            if (ElementIndex != INDEX_NONE)
            {
                return &Elements[ElementIndex].Value;
            }
            return nullptr;
        }

        template<typename ComparableKey>
        [[nodiscard]] const ElementType* FindByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            return const_cast<TSparseSet*>(this)->FindByHash(KeyHash, Key);
        }

        /**
         * @brief Check if an element with the given key exists
         * @param Key  The key to search for
         * @return true if an element with the key exists
         */
        [[nodiscard]] OLO_FINLINE bool Contains(KeyInitType Key) const
        {
            return FindId(Key).IsValidId();
        }

        /**
         * @brief Check if an element with the given key exists, using a precomputed hash
         *
         * @see Class documentation section on ByHash() functions
         * @param KeyHash  A precomputed hash value
         * @param Key  The key to search for
         * @return true if an element with the key exists
         */
        template<typename ComparableKey>
        [[nodiscard]] inline bool ContainsByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            OLO_CORE_ASSERT(KeyHash == KeyFuncs::GetKeyHash(Key), "Hash mismatch in ContainsByHash");
            return FindIndexByHash(KeyHash, Key) != INDEX_NONE;
        }

        /**
         * @brief Find an arbitrary element
         * @return Pointer to an element or nullptr if empty
         */
        [[nodiscard]] ElementType* FindArbitraryElement()
        {
            i32 Result = Elements.FindArbitraryElementIndex();
            return (Result != INDEX_NONE) ? &Elements[Result].Value : nullptr;
        }

        [[nodiscard]] const ElementType* FindArbitraryElement() const
        {
            return const_cast<TSparseSet*>(this)->FindArbitraryElement();
        }

        // ========================================================================
        // Set Operations
        // ========================================================================

        /**
         * @brief Compute union of two sets
         * @param OtherSet  The other set
         * @return A new set containing elements from both sets
         */
        [[nodiscard]] TSparseSet Union(const TSparseSet& OtherSet) const
        {
            TSparseSet Result;
            Result.Reserve(Num() + OtherSet.Num());

            for (const ElementType& Element : *this)
            {
                Result.Add(Element);
            }
            for (const ElementType& Element : OtherSet)
            {
                Result.Add(Element);
            }

            return Result;
        }

        /**
         * @brief Compute intersection of two sets
         * @param OtherSet  The other set
         * @return A new set containing elements in both sets
         */
        [[nodiscard]] TSparseSet Intersect(const TSparseSet& OtherSet) const
        {
            TSparseSet Result;

            for (const ElementType& Element : *this)
            {
                if (OtherSet.Contains(KeyFuncs::GetSetKey(Element)))
                {
                    Result.Add(Element);
                }
            }

            return Result;
        }

        /**
         * @brief Compute difference of two sets
         * @param OtherSet  The other set
         * @return A new set containing elements in this set but not in OtherSet
         */
        [[nodiscard]] TSparseSet Difference(const TSparseSet& OtherSet) const
        {
            TSparseSet Result;

            for (const ElementType& Element : *this)
            {
                if (!OtherSet.Contains(KeyFuncs::GetSetKey(Element)))
                {
                    Result.Add(Element);
                }
            }

            return Result;
        }

        /**
         * @brief Check if this set is a subset of another
         * @param OtherSet  The other set
         * @return true if all elements of this set are in OtherSet
         */
        [[nodiscard]] bool IsSubsetOf(const TSparseSet& OtherSet) const
        {
            for (const ElementType& Element : *this)
            {
                if (!OtherSet.Contains(KeyFuncs::GetSetKey(Element)))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Determine whether the specified set is entirely included within this set
         * @param OtherSet  Set to check
         * @return True if the other set is entirely included in this set
         */
        [[nodiscard]] bool Includes(const TSparseSet& OtherSet) const
        {
            if (OtherSet.Num() <= Num())
            {
                for (TConstIterator It(OtherSet); It; ++It)
                {
                    if (!Contains(KeyFuncs::GetSetKey(*It)))
                    {
                        return false;
                    }
                }
                return true;
            }
            // Not possible to include if it is bigger than us
            return false;
        }

        /** @return a TArray of the elements */
        [[nodiscard]] TArray<ElementType> Array() const
        {
            TArray<ElementType> Result;
            Result.Reserve(Num());
            for (TConstIterator It(*this); It; ++It)
            {
                Result.Add(*It);
            }
            return Result;
        }

        /**
         * @brief Check that the specified address is not part of an element within the container
         * @param Addr  The address to check
         */
        OLO_FINLINE void CheckAddress(const ElementType* Addr) const
        {
            Elements.CheckAddress(Addr);
        }

        // ========================================================================
        // Iterators
        // ========================================================================

        /** Base iterator class */
        template<bool bConst>
        class TBaseIterator
        {
          public:
            using SetType = std::conditional_t<bConst, const TSparseSet, TSparseSet>;
            using IteratedElementType = std::conditional_t<bConst, const ElementType, ElementType>;

          private:
            SetType& Set;
            typename ElementArrayType::template TBaseIterator<bConst> ElementIt;

          public:
            [[nodiscard]] explicit TBaseIterator(SetType& InSet)
                : Set(InSet), ElementIt([&InSet]()
                                        {
                    if constexpr (bConst)
                    {
                        return InSet.Elements.CreateConstIterator();
                    }
                    else
                    {
                        return InSet.Elements.CreateIterator();
                    } }())
            {
            }

            [[nodiscard]] explicit TBaseIterator(SetType& InSet, i32 StartIndex)
                : Set(InSet), ElementIt(InSet.Elements, StartIndex)
            {
            }

            OLO_FINLINE TBaseIterator& operator++()
            {
                ++ElementIt;
                return *this;
            }

            [[nodiscard]] OLO_FINLINE explicit operator bool() const
            {
                return static_cast<bool>(ElementIt);
            }

            [[nodiscard]] OLO_FINLINE bool operator!() const
            {
                return !static_cast<bool>(ElementIt);
            }

            [[nodiscard]] OLO_FINLINE FSetElementId GetId() const
            {
                return FSetElementId::FromInteger(ElementIt.GetIndex());
            }

            [[nodiscard]] OLO_FINLINE i32 GetIndex() const
            {
                return ElementIt.GetIndex();
            }

            [[nodiscard]] OLO_FINLINE IteratedElementType& operator*() const
            {
                return (*ElementIt).Value;
            }

            [[nodiscard]] OLO_FINLINE IteratedElementType* operator->() const
            {
                return &(*ElementIt).Value;
            }

            [[nodiscard]] OLO_FINLINE bool operator==(const TBaseIterator& Other) const
            {
                return GetIndex() == Other.GetIndex();
            }

            [[nodiscard]] OLO_FINLINE bool operator!=(const TBaseIterator& Other) const
            {
                return !(*this == Other);
            }

            /** Remove current element (mutable iterator only) */
            void RemoveCurrent()
                requires(!bConst)
            {
                Set.Remove(GetId());
            }
        };

        using TIterator = TBaseIterator<false>;
        using TConstIterator = TBaseIterator<true>;
        using TRangedForIterator = TIterator;
        using TRangedForConstIterator = TConstIterator;

        /** The base type of key iterators - iterates over elements with a specific key */
        template<bool bConst>
        class TBaseKeyIterator
        {
          private:
            using SetType = std::conditional_t<bConst, const TSparseSet, TSparseSet>;
            using IteratedElementType = std::conditional_t<bConst, const ElementType, ElementType>;
            using ReferenceOrValueType = typename TTypeTraits<typename KeyFuncs::KeyType>::ConstPointerType;

          public:
            /** Type used for key arguments - ensures proper lifetime handling */
            using KeyArgumentType = KeyInitType;

            /** Initialization constructor */
            [[nodiscard]] inline TBaseKeyIterator(SetType& InSet, KeyArgumentType InKey)
                : Set(InSet), Key(InKey), Index(INDEX_NONE)
            {
                if (Set.HashSize)
                {
                    NextIndex = Set.GetTypedHash(KeyFuncs::GetKeyHash(Key)).AsInteger();
                    ++(*this);
                }
                else
                {
                    NextIndex = INDEX_NONE;
                }
            }

            /** Advances the iterator to the next element */
            inline TBaseKeyIterator& operator++()
            {
                Index = NextIndex;

                while (Index != INDEX_NONE)
                {
                    NextIndex = Set.Elements[Index].HashNextId.AsInteger();
                    OLO_CORE_ASSERT(Index != NextIndex, "Circular hash chain detected");

                    if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Set.Elements[Index].Value), Key))
                    {
                        break;
                    }

                    Index = NextIndex;
                }
                return *this;
            }

            /** Conversion to "bool" returning true if the iterator is valid */
            [[nodiscard]] OLO_FINLINE explicit operator bool() const
            {
                return Index != INDEX_NONE;
            }

            /** Inverse of the "bool" operator */
            [[nodiscard]] OLO_FINLINE bool operator!() const
            {
                return !static_cast<bool>(*this);
            }

            /** @return the element id */
            [[nodiscard]] OLO_FINLINE FSetElementId GetId() const
            {
                return FSetElementId::FromInteger(Index);
            }

            [[nodiscard]] OLO_FINLINE IteratedElementType* operator->() const
            {
                return &Set.Elements[Index].Value;
            }

            [[nodiscard]] OLO_FINLINE IteratedElementType& operator*() const
            {
                return Set.Elements[Index].Value;
            }

          protected:
            SetType& Set;
            KeyInitType Key;
            i32 Index;
            i32 NextIndex;
        };

        /** Used to iterate over elements of a const TSparseSet with a specific key */
        class TConstKeyIterator : public TBaseKeyIterator<true>
        {
          private:
            using Super = TBaseKeyIterator<true>;

          public:
            using KeyArgumentType = typename Super::KeyArgumentType;

            [[nodiscard]] OLO_FINLINE TConstKeyIterator(const TSparseSet& InSet, KeyArgumentType InKey)
                : Super(InSet, InKey)
            {
            }
        };

        /** Used to iterate over elements of a TSparseSet with a specific key */
        class TKeyIterator : public TBaseKeyIterator<false>
        {
          private:
            using Super = TBaseKeyIterator<false>;

          public:
            using KeyArgumentType = typename Super::KeyArgumentType;

            [[nodiscard]] OLO_FINLINE TKeyIterator(TSparseSet& InSet, KeyArgumentType InKey)
                : Super(InSet, InKey)
            {
            }

            /** Removes the current element from the set */
            inline void RemoveCurrent()
            {
                this->Set.RemoveByIndex(this->Index);
                this->Index = INDEX_NONE;
            }
        };

        /** Create an iterator */
        [[nodiscard]] TIterator CreateIterator()
        {
            return TIterator(*this);
        }

        /** Create a const iterator */
        [[nodiscard]] TConstIterator CreateConstIterator() const
        {
            return TConstIterator(*this);
        }

        /** Range-based for loop support */
        [[nodiscard]] TIterator begin()
        {
            return TIterator(*this);
        }
        [[nodiscard]] TConstIterator begin() const
        {
            return TConstIterator(*this);
        }
        [[nodiscard]] TIterator end()
        {
            return TIterator(*this, GetMaxIndex());
        }
        [[nodiscard]] TConstIterator end() const
        {
            return TConstIterator(*this, GetMaxIndex());
        }

        // Sets are deliberately prevented from being hashed or compared, because this would hide
        // potentially major performance problems behind default operations.
        friend u32 GetTypeHash(const TSparseSet&) = delete;
        friend bool operator==(const TSparseSet&, const TSparseSet&) = delete;
        friend bool operator!=(const TSparseSet&, const TSparseSet&) = delete;

        // ========================================================================
        // Debug / Diagnostics
        // ========================================================================

        /**
         * @brief Describes the set's contents through an output device
         * @param Ar  The output device to describe the set's contents through
         */
        /**
         * @brief Describes the set's contents through an output device
         *
         * @note This method requires FOutputDevice to be fully defined.
         *       Currently stubbed until FOutputDevice is implemented.
         *       Future signature: void Dump(FOutputDevice& Ar)
         */
        void Dump()
        {
            // TODO: Implement when FOutputDevice is available
            // Ar.Logf(TEXT("TSparseSet: %i elements, %i hash slots"), Elements.Num(), HashSize);
            // for (i32 HashIndex = 0; HashIndex < HashSize; ++HashIndex)
            // {
            //     // Count the number of elements in this hash bucket
            //     i32 NumElementsInBucket = 0;
            //     for (FSetElementId ElementId = GetTypedHash(HashIndex);
            //          ElementId.IsValidId();
            //          ElementId = Elements[ElementId.AsInteger()].HashNextId)
            //     {
            //         NumElementsInBucket++;
            //     }
            //     Ar.Logf(TEXT("   Hash[%i] = %i"), HashIndex, NumElementsInBucket);
            // }
        }

        /**
         * @brief Verifies that all element ids in the hash chain for the given key are valid
         * @param Key  The key to verify
         * @return true if all element ids in the hash chain are valid
         */
        [[nodiscard]] bool VerifyHashElementsKey(KeyInitType Key) const
        {
            bool bResult = true;
            if (Elements.Num())
            {
                // Iterate over all elements for the hash entry of the given key
                // and verify that the ids are valid
                FSetElementId ElementId = GetTypedHash(KeyFuncs::GetKeyHash(Key));
                while (ElementId.IsValidId())
                {
                    if (!IsValidId(ElementId))
                    {
                        bResult = false;
                        break;
                    }
                    ElementId = Elements[ElementId.AsInteger()].HashNextId;
                }
            }
            return bResult;
        }

        // ========================================================================
        // Memory Image Support
        // ========================================================================

        /** Write the set to a memory image for frozen data */
        void WriteMemoryImage(FMemoryImageWriter& Writer) const
        {
            if constexpr (TAllocatorTraits<Allocator>::SupportsFreezeMemoryImage && THasTypeLayout<ElementType>::Value)
            {
                this->Elements.WriteMemoryImage(Writer);
                this->Hash.WriteMemoryImage(Writer, StaticGetTypeLayoutDesc<FSetElementId>(), this->HashSize);
                Writer.WriteBytes(this->HashSize);
            }
            else
            {
                Writer.WriteBytes(TSparseSet());
            }
        }

        /** Copy from frozen data to unfrozen */
        void CopyUnfrozen(const FMemoryUnfreezeContent& Context, void* Dst) const
        {
            if constexpr (TAllocatorTraits<Allocator>::SupportsFreezeMemoryImage && THasTypeLayout<ElementType>::Value)
            {
                TSparseSet* DstObject = static_cast<TSparseSet*>(Dst);
                this->Elements.CopyUnfrozen(Context, &DstObject->Elements);

                ::new (static_cast<void*>(&DstObject->Hash)) HashType();
                DstObject->Hash.ResizeAllocation(0, this->HashSize, sizeof(FSetElementId));
                std::memcpy(DstObject->Hash.GetAllocation(), this->Hash.GetAllocation(), sizeof(FSetElementId) * this->HashSize);
                DstObject->HashSize = this->HashSize;
            }
            else
            {
                ::new (Dst) TSparseSet();
            }
        }

        /** Append hash for type layout */
        static void AppendHash(const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
        {
            ElementArrayType::AppendHash(LayoutParams, Hasher);
        }

      private:
        // ========================================================================
        // Internal Helpers
        // ========================================================================

        /** Get hash bucket reference */
        [[nodiscard]] OLO_FINLINE FSetElementId& GetTypedHash(u32 HashIndex)
        {
            return SparseSetPrivate::GetTypedHash(Hash, static_cast<i32>(HashIndex), HashSize);
        }

        [[nodiscard]] OLO_FINLINE const FSetElementId& GetTypedHash(u32 HashIndex) const
        {
            return SparseSetPrivate::GetTypedHash(Hash, static_cast<i32>(HashIndex), HashSize);
        }

        /** Find element index by hash and key */
        template<typename ComparableKey>
        [[nodiscard]] i32 FindIndexByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            if (Elements.Num() == 0)
            {
                return INDEX_NONE;
            }

            FSetElementId* HashPtr = reinterpret_cast<FSetElementId*>(Hash.GetAllocation());
            i32 ElementIndex = HashPtr[KeyHash & (HashSize - 1)].AsInteger();

            while (ElementIndex != INDEX_NONE)
            {
                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Elements[ElementIndex].Value), Key))
                {
                    return ElementIndex;
                }
                ElementIndex = Elements[ElementIndex].HashNextId.AsInteger();
            }

            return INDEX_NONE;
        }

        /** Try to replace an existing element with same key */
        bool TryReplaceExisting(u32 KeyHash, SetElementType& Element, i32& InOutElementIndex, bool* bIsAlreadyInSetPtr)
        {
            bool bIsAlreadyInSet = false;

            if constexpr (!KeyFuncs::bAllowDuplicateKeys)
            {
                // Check for existing element with same key
                if (Elements.Num() != 1)
                {
                    i32 ExistingIndex = FindIndexByHash(KeyHash, KeyFuncs::GetSetKey(Element.Value));
                    bIsAlreadyInSet = ExistingIndex != INDEX_NONE;

                    if (bIsAlreadyInSet)
                    {
                        // Replace existing element
                        MoveByRelocate(Elements[ExistingIndex].Value, Element.Value);
                        Elements.RemoveAtUninitialized(InOutElementIndex);
                        InOutElementIndex = ExistingIndex;
                    }
                }
            }

            if (bIsAlreadyInSetPtr)
            {
                *bIsAlreadyInSetPtr = bIsAlreadyInSet;
            }
            return bIsAlreadyInSet;
        }

        /** Link element into hash chain or trigger rehash */
        inline void RehashOrLink(u32 KeyHash, SetElementType& Element, i32 ElementIndex)
        {
            if (!ConditionalRehash(Elements.Num(), EAllowShrinking::No))
            {
                LinkElement(ElementIndex, Element, KeyHash);
            }
        }

        /** Link element into hash chain */
        inline void LinkElement(i32 ElementIndex, SetElementType& Element, u32 KeyHash) const
        {
            Element.HashIndex = KeyHash & (HashSize - 1);
            Element.HashNextId = GetTypedHash(KeyHash);
            reinterpret_cast<FSetElementId*>(Hash.GetAllocation())[Element.HashIndex] = FSetElementId::FromInteger(ElementIndex);
        }

        /** Unlink all elements from hash */
        void UnhashElements()
        {
            if (HashSize > 0)
            {
                FSetElementId* HashPtr = reinterpret_cast<FSetElementId*>(Hash.GetAllocation());
                for (i32 i = 0; i < HashSize; ++i)
                {
                    HashPtr[i] = FSetElementId();
                }
            }
        }

        /** Check if rehash is needed */
        [[nodiscard]] bool ShouldRehash(i32 NumElements, i32 DesiredHashSize, bool bAllowShrinking) const
        {
            if (DesiredHashSize != HashSize)
            {
                if (DesiredHashSize > HashSize || bAllowShrinking)
                {
                    return true;
                }
            }
            return false;
        }

        /** Conditionally rehash based on element count */
        bool ConditionalRehash(i32 NumElements, EAllowShrinking AllowShrinking)
        {
            const i32 DesiredHashSize = Allocator::GetNumberOfHashBuckets(NumElements);

            if (ShouldRehash(NumElements, DesiredHashSize, AllowShrinking == EAllowShrinking::Yes))
            {
                HashSize = DesiredHashSize;
                Rehash();
                return true;
            }
            return false;
        }

        /** Rebuild the hash table */
        void Rehash()
        {
            SparseSetPrivate::Rehash(Hash, HashSize);

            if (HashSize > 0)
            {
                // Relink all elements into hash
                for (typename ElementArrayType::TIterator It(Elements); It; ++It)
                {
                    SetElementType& Element = *It;
                    u32 KeyHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Element.Value));
                    LinkElement(It.GetIndex(), Element, KeyHash);
                }
            }
        }

        /** Remove element by index */
        void RemoveByIndex(i32 ElementIndex)
        {
            OLO_CORE_ASSERT(Elements.IsValidIndex(ElementIndex), "Invalid element index");

            const SetElementType& ElementBeingRemoved = Elements[ElementIndex];

            // Remove from hash chain
            FSetElementId* HashPtr = reinterpret_cast<FSetElementId*>(Hash.GetAllocation());
            FSetElementId* NextElementIndexIter = &HashPtr[ElementBeingRemoved.HashIndex];

            while (NextElementIndexIter->IsValidId())
            {
                i32 NextElementIndex = NextElementIndexIter->AsInteger();

                if (NextElementIndex == ElementIndex)
                {
                    *NextElementIndexIter = ElementBeingRemoved.HashNextId;
                    break;
                }

                NextElementIndexIter = &Elements[NextElementIndex].HashNextId;
            }

            // Remove from elements array
            Elements.RemoveAt(ElementIndex);
        }

        /** Remove implementation */
        template<typename ComparableKey>
        i32 RemoveImpl(u32 KeyHash, const ComparableKey& Key)
        {
            i32 NumRemovedElements = 0;

            FSetElementId* NextElementId = &GetTypedHash(KeyHash);
            while (NextElementId->IsValidId())
            {
                const i32 ElementIndex = NextElementId->AsInteger();
                SetElementType& Element = Elements[ElementIndex];

                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Element.Value), Key))
                {
                    RemoveByIndex(ElementIndex);
                    NumRemovedElements++;

                    if constexpr (!KeyFuncs::bAllowDuplicateKeys)
                    {
                        break;
                    }
                }
                else
                {
                    NextElementId = &Element.HashNextId;
                }
            }

            return NumRemovedElements;
        }

        /** Extracts the element value from the set's element structure and passes it to the user provided comparison class. */
        template<typename PredicateClass>
        class FElementCompareClass
        {
            const PredicateClass& Predicate;

          public:
            [[nodiscard]] OLO_FINLINE FElementCompareClass(const PredicateClass& InPredicate)
                : Predicate(InPredicate)
            {
            }

            [[nodiscard]] OLO_FINLINE bool operator()(const SetElementType& A, const SetElementType& B) const
            {
                return Predicate(A.Value, B.Value);
            }
        };
    };

    // ============================================================================
    // Freeze Namespace Functions
    // ============================================================================

    namespace Freeze
    {
        template<typename ElementType, typename KeyFuncs, typename Allocator>
        void IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer, const TSparseSet<ElementType, KeyFuncs, Allocator>& Object, const FTypeLayoutDesc&)
        {
            Object.WriteMemoryImage(Writer);
        }

        template<typename ElementType, typename KeyFuncs, typename Allocator>
        u32 IntrinsicUnfrozenCopy(const FMemoryUnfreezeContent& Context, const TSparseSet<ElementType, KeyFuncs, Allocator>& Object, void* OutDst)
        {
            Object.CopyUnfrozen(Context, OutDst);
            return sizeof(Object);
        }

        template<typename ElementType, typename KeyFuncs, typename Allocator>
        u32 IntrinsicAppendHash(const TSparseSet<ElementType, KeyFuncs, Allocator>*, const FTypeLayoutDesc& TypeDesc, const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
        {
            TSparseSet<ElementType, KeyFuncs, Allocator>::AppendHash(LayoutParams, Hasher);
            return DefaultAppendHash(TypeDesc, LayoutParams, Hasher);
        }
    } // namespace Freeze

    // ============================================================================
    // Serialization
    // ============================================================================

    /** Serializer */
    template<typename ElementType, typename KeyFuncs, typename Allocator>
    FArchive& operator<<(FArchive& Ar, TSparseSet<ElementType, KeyFuncs, Allocator>& Set)
    {
        // Load the set's new elements
        Ar << Set.Elements;

        if (Ar.IsLoading() || (Ar.IsModifyingWeakAndStrongReferences() && !Ar.IsSaving()))
        {
            // Free the old hash
            Set.Hash.ResizeAllocation(0, 0, sizeof(FSetElementId));
            Set.HashSize = 0;

            // Rehash the newly loaded elements
            Set.ConditionalRehash(Set.Elements.Num(), EAllowShrinking::No);
        }

        return Ar;
    }

    /** Structured archive serializer */
    template<typename ElementType, typename KeyFuncs, typename Allocator>
    void operator<<(FStructuredArchive::FSlot Slot, TSparseSet<ElementType, KeyFuncs, Allocator>& Set)
    {
        Slot << Set.Elements;

        if (Slot.GetUnderlyingArchive().IsLoading() || (Slot.GetUnderlyingArchive().IsModifyingWeakAndStrongReferences() && !Slot.GetUnderlyingArchive().IsSaving()))
        {
            // Free the old hash
            Set.Hash.ResizeAllocation(0, 0, sizeof(FSetElementId));
            Set.HashSize = 0;

            // Rehash the newly loaded elements
            Set.ConditionalRehash(Set.Elements.Num(), EAllowShrinking::No);
        }
    }

    // ============================================================================
    // TSparseSetPrivateFriend - Friend struct for serialization access
    // ============================================================================

    struct TSparseSetPrivateFriend
    {
        /** Serializer. */
        template<typename ElementType, typename KeyFuncs, typename Allocator>
        static FArchive& Serialize(FArchive& Ar, TSparseSet<ElementType, KeyFuncs, Allocator>& Set)
        {
            // Load the set's new elements.
            Ar << Set.Elements;

            if (Ar.IsLoading() || (Ar.IsModifyingWeakAndStrongReferences() && !Ar.IsSaving()))
            {
                // Free the old hash.
                Set.Hash.ResizeAllocation(0, 0, sizeof(FSetElementId));
                Set.HashSize = 0;

                // Hash the newly loaded elements.
                Set.ConditionalRehash(Set.Elements.Num(), EAllowShrinking::No);
            }

            return Ar;
        }

        /** Structured archive serializer. */
        template<typename ElementType, typename KeyFuncs, typename Allocator>
        static void SerializeStructured(FStructuredArchive::FSlot Slot, TSparseSet<ElementType, KeyFuncs, Allocator>& Set)
        {
            Slot << Set.Elements;

            if (Slot.GetUnderlyingArchive().IsLoading() || (Slot.GetUnderlyingArchive().IsModifyingWeakAndStrongReferences() && !Slot.GetUnderlyingArchive().IsSaving()))
            {
                // Free the old hash.
                Set.Hash.ResizeAllocation(0, 0, sizeof(FSetElementId));
                Set.HashSize = 0;

                // Hash the newly loaded elements.
                Set.ConditionalRehash(Set.Elements.Num(), EAllowShrinking::No);
            }
        }

        /** Legacy comparison - also tests whether elements were added in same order */
        template<typename ElementType, typename KeyFuncs, typename Allocator>
        [[nodiscard]] static bool LegacyCompareEqual(const TSparseSet<ElementType, KeyFuncs, Allocator>& A, const TSparseSet<ElementType, KeyFuncs, Allocator>& B)
        {
            return A.Elements == B.Elements;
        }
    };

    // Alias for compatibility with Map.h
    using TSetPrivateFriend = TSparseSetPrivateFriend;

    // ============================================================================
    // Legacy Comparison Functions
    // ============================================================================

    /** Legacy equality comparison - also tests whether elements were added in same order */
    template<typename ElementType, typename KeyFuncs, typename Allocator>
    [[nodiscard]] bool LegacyCompareEqual(const TSparseSet<ElementType, KeyFuncs, Allocator>& A, const TSparseSet<ElementType, KeyFuncs, Allocator>& B)
    {
        return TSparseSetPrivateFriend::LegacyCompareEqual(A, B);
    }

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    [[nodiscard]] bool LegacyCompareNotEqual(const TSparseSet<ElementType, KeyFuncs, Allocator>& A, const TSparseSet<ElementType, KeyFuncs, Allocator>& B)
    {
        return !TSparseSetPrivateFriend::LegacyCompareEqual(A, B);
    }

    // ============================================================================
    // Deduction Guide
    // ============================================================================

    // TODO: Re-enable when TElementType trait is ported
    // template <typename RangeType>
    // TSparseSet(RangeType&&) -> TSparseSet<typename TElementType<RangeType>::Type>;

    // ============================================================================
    // TIsSparseSet Specializations
    // ============================================================================

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsSparseSet<TSparseSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsSparseSet<const TSparseSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsSparseSet<volatile TSparseSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsSparseSet<const volatile TSparseSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

} // namespace OloEngine
