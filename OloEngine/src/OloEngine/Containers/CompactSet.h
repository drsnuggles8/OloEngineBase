#pragma once

/**
 * @file CompactSet.h
 * @brief Compact hash set implementation
 *
 * TCompactSet is an alternative set implementation that uses a more compact
 * memory layout compared to TSparseSet. Key differences:
 *
 * - Elements are stored contiguously with no holes
 * - Memory layout: [Elements][HashCount][CollisionList][HashTable]
 * - Index type adapts to element count (u8/u16/u32)
 * - Removal moves the last element into the removed slot
 *
 * This provides better cache locality and lower memory overhead for most use cases,
 * but does not preserve element order on removal.
 *
 * Ported from Unreal Engine's Containers/CompactSet.h.inl
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Containers/ArrayView.h"
#include "OloEngine/Containers/CompactSetBase.h"
#include "OloEngine/Containers/SetElement.h"
#include "OloEngine/Containers/Array.h" // For EAllowShrinking
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Templates/TypeHash.h"
#include "OloEngine/Templates/RetainedRef.h"
#include "OloEngine/Memory/MemoryOps.h"
#include <algorithm>
#include <initializer_list>
#include <type_traits>

namespace OloEngine
{
    // Forward declarations
    template<typename KeyType, typename ValueType>
    struct TPair;

    /**
     * @class TCompactSet
     * @brief A compact hash set with customizable key functions
     *
     * The compact set stores elements contiguously and uses an adaptive index type
     * based on element count. This provides better memory efficiency than TSparseSet
     * for most use cases.
     *
     * @tparam InElementType The element type stored in the set
     * @tparam KeyFuncs      Key functions for hashing and comparison (default: use element as key)
     * @tparam Allocator     Allocator policy (default: FDefaultAllocator)
     */
    template<
        typename InElementType,
        typename KeyFuncs = DefaultKeyFuncs<InElementType>,
        typename Allocator = FDefaultAllocator>
    class TCompactSet
        : public TCompactSetBase<Allocator>
    {
      public:
        using ElementType = InElementType;
        using KeyFuncsType = KeyFuncs;
        using AllocatorType = Allocator;

      private:
        using Super = TCompactSetBase<Allocator>;
        using typename Super::HashCountType;
        using typename Super::SizeType;
        using USizeType = std::make_unsigned_t<SizeType>;
        using KeyInitType = typename KeyFuncs::KeyInitType;
        using ElementInitType = typename KeyFuncs::ElementInitType;

      public:
        // Bring base class public members into scope
        using Super::GetMaxIndex;
        using Super::IsEmpty;
        using Super::Max;
        using Super::Num;
        // ====================================================================
        // Iterator Types
        // ====================================================================

        /**
         * @class TBaseIterator
         * @brief Base iterator for the compact set
         */
        template<bool bConst>
        class TBaseIterator
        {
          private:
            using SetType = std::conditional_t<bConst, const TCompactSet, TCompactSet>;

          public:
            using ElementItType = std::conditional_t<bConst, const ElementType, ElementType>;

            [[nodiscard]] OLO_FINLINE TBaseIterator(SetType& InSet)
                : TBaseIterator(InSet, 0)
            {
            }

            [[nodiscard]] inline TBaseIterator(SetType& InSet, SizeType StartIndex)
                : Set(InSet), Index(StartIndex)
#if !OLO_BUILD_SHIPPING
                  ,
                  InitialNum(InSet.Num())
#endif
            {
                OLO_CORE_ASSERT(StartIndex >= 0 && StartIndex <= InSet.Num(), "Invalid start index");
            }

            [[nodiscard]] OLO_FINLINE ElementItType& operator*() const
            {
                return Set[FSetElementId::FromInteger(Index)];
            }

            [[nodiscard]] OLO_FINLINE ElementItType* operator->() const
            {
                return &Set[FSetElementId::FromInteger(Index)];
            }

            OLO_FINLINE TBaseIterator& operator++()
            {
#if !OLO_BUILD_SHIPPING
                OLO_CORE_ASSERT(Set.Num() >= InitialNum,
                                "Set modified during iteration (elements removed)");
#endif
                ++Index;
                return *this;
            }

            [[nodiscard]] OLO_FINLINE explicit operator bool() const
            {
                return Set.IsValidId(GetId());
            }

            [[nodiscard]] inline bool operator==(const TBaseIterator& Rhs) const
            {
#if !OLO_BUILD_SHIPPING
                OLO_CORE_ASSERT(&Set == &Rhs.Set, "Comparing iterators from different sets");
#endif
                return Index == Rhs.Index;
            }

            [[nodiscard]] OLO_FINLINE bool operator!=(const TBaseIterator& Rhs) const
            {
                return !(*this == Rhs);
            }

            [[nodiscard]] OLO_FINLINE FSetElementId GetId() const
            {
                return FSetElementId::FromInteger(Index);
            }

          protected:
            SetType& Set;
            SizeType Index;
#if !OLO_BUILD_SHIPPING
            SizeType InitialNum;
#endif
        };

        /** Const iterator type */
        using TConstIterator = TBaseIterator<true>;

        /**
         * @class TIterator
         * @brief Mutable iterator that supports RemoveCurrent
         */
        class TIterator : public TBaseIterator<false>
        {
          private:
            using Super = TBaseIterator<false>;

          public:
            using Super::Super;

            /** Removes the current element from the set */
            inline void RemoveCurrent()
            {
                Super::Set.RemoveById(Super::GetId());
                --Super::Index; // Compensate since last element was moved here
#if !OLO_BUILD_SHIPPING
                --Super::InitialNum;
#endif
            }
        };

        /**
         * @class TBaseKeyIterator
         * @brief Base iterator for finding elements by key (handles hash collisions)
         */
        template<bool bConst>
        class TBaseKeyIterator
        {
          private:
            using SetType = std::conditional_t<bConst, const TCompactSet, TCompactSet>;
            using ItElementType = std::conditional_t<bConst, const ElementType, ElementType>;
            using ReferenceOrValueType = typename TTypeTraits<typename KeyFuncs::KeyType>::ConstPointerType;

          public:
            // Use TRetainedRef for reference types to avoid dangling references
            using KeyArgumentType = std::conditional_t<
                std::is_reference_v<ReferenceOrValueType>,
                TRetainedRef<std::remove_reference_t<ReferenceOrValueType>>,
                KeyInitType>;

            /** Initialization constructor */
            [[nodiscard]] inline TBaseKeyIterator(SetType& InSet, KeyArgumentType InKey)
                : Set(InSet), Key(InKey), HashTable(InSet.Num() ? Set.GetConstHashTableView() : FConstCompactHashTableView()), Index(INDEX_NONE), NextIndex(InSet.Num() ? HashTable.GetFirst(KeyFuncs::GetKeyHash(Key)) : INDEX_NONE)
#if !OLO_BUILD_SHIPPING
                  ,
                  InitialNum(InSet.Num())
#endif
            {
                ++(*this);
            }

            /** Advances the iterator to the next matching element */
            inline TBaseKeyIterator& operator++()
            {
                const i32 SetNum = Set.Num();
#if !OLO_BUILD_SHIPPING
                OLO_CORE_ASSERT(SetNum >= InitialNum,
                                "Set modified during iteration (elements removed)");
#endif
                Index = NextIndex;

                while (Index != INDEX_NONE)
                {
                    NextIndex = HashTable.GetNext(Index, SetNum);
                    OLO_CORE_ASSERT(static_cast<i32>(Index) != static_cast<i32>(NextIndex), "Hash chain cycle detected");

                    if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Set[GetId()]), Key))
                    {
                        break;
                    }

                    Index = NextIndex;
                }

                return *this;
            }

            /** conversion to "bool" returning true if the iterator is valid */
            [[nodiscard]] OLO_FINLINE explicit operator bool() const
            {
                return Index != INDEX_NONE;
            }

            /** @return the element ID */
            [[nodiscard]] OLO_FINLINE FSetElementId GetId() const
            {
                return FSetElementId::FromInteger(Index);
            }

            [[nodiscard]] OLO_FINLINE ItElementType* operator->() const
            {
                return &Set[GetId()];
            }

            [[nodiscard]] OLO_FINLINE ItElementType& operator*() const
            {
                return Set[GetId()];
            }

          protected:
            SetType& Set;
            ReferenceOrValueType Key;
            FConstCompactHashTableView HashTable;
            SizeType Index;
            SizeType NextIndex;
#if !OLO_BUILD_SHIPPING
            SizeType InitialNum;
#endif
        };

        /** Const key iterator */
        using TConstKeyIterator = TBaseKeyIterator<true>;

        /**
         * @class TKeyIterator
         * @brief Mutable key iterator that supports RemoveCurrent
         */
        class TKeyIterator : public TBaseKeyIterator<false>
        {
          private:
            using Super = TBaseKeyIterator<false>;

          public:
            using KeyArgumentType = typename Super::KeyArgumentType;
            using Super::Super;

            /** Removes the current element from the set */
            inline void RemoveCurrent()
            {
                Super::Set.RemoveById(Super::GetId());
#if !OLO_BUILD_SHIPPING
                --Super::InitialNum;
#endif

                // If the next element was the last in the set then it will get remapped to the current index
                if (Super::NextIndex == static_cast<SizeType>(Super::Set.Num()))
                {
                    Super::NextIndex = Super::Index;
                }

                Super::Index = INDEX_NONE;
            }
        };

        // ====================================================================
        // Constructors & Destructor
        // ====================================================================

        /** Default constructor */
        [[nodiscard]] TCompactSet() = default;

        /** Consteval constructor */
        [[nodiscard]] explicit consteval TCompactSet(EConstEval)
            : Super(ConstEval)
        {
        }

        /** Destructor */
        ~TCompactSet()
        {
            // TCompactSet can only be used with trivially relocatable types
            static_assert(TIsTriviallyRelocatable_V<InElementType>,
                          "TCompactSet can only be used with trivially relocatable types");
            Empty(0);
        }

        /** Copy constructor */
        TCompactSet(const TCompactSet& Other)
        {
            *this = Other;
        }

        /** Move constructor */
        TCompactSet(TCompactSet&& Other) noexcept
        {
            *this = MoveTemp(Other);
        }

        /** Initializer list constructor */
        TCompactSet(std::initializer_list<ElementType> InitList)
        {
            Append(InitList);
        }

        /** TArrayView constructor */
        template<typename ViewElementType>
        [[nodiscard]] explicit TCompactSet(TArrayView<ViewElementType> InArrayView)
        {
            Append(InArrayView);
        }

        /** TArray move constructor */
        [[nodiscard]] explicit TCompactSet(TArray<ElementType>&& InArray)
        {
            Append(MoveTemp(InArray));
        }

        /** Constructor for moving elements from a TCompactSet with a different allocator */
        template<typename OtherAllocator>
        [[nodiscard]] TCompactSet(TCompactSet<ElementType, KeyFuncs, OtherAllocator>&& Other)
        {
            Append(MoveTemp(Other));
        }

        /** Constructor for copying elements from a TCompactSet with a different allocator */
        template<typename OtherAllocator>
        [[nodiscard]] TCompactSet(const TCompactSet<ElementType, KeyFuncs, OtherAllocator>& Other)
        {
            Append(Other);
        }

        // ====================================================================
        // Intrusive TOptional Support
        // ====================================================================

        static constexpr bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TCompactSet;

        /** Constructor for intrusive optional unset state */
        [[nodiscard]] explicit TCompactSet(FIntrusiveUnsetOptionalState Tag)
            : Super(Tag)
        {
        }

        // ====================================================================
        // Assignment Operators
        // ====================================================================

        /** Copy assignment */
        TCompactSet& operator=(const TCompactSet& Other)
        {
            if (this != &Other)
            {
                // This could either take the full memory size of the Copy and prevent a rehash or
                // only allocate the required set but have to hash everything again (i.e. perf vs memory)
                // Could try a middle ground of allowing extra memory if it's within slack margins.
                // Going for memory savings for now

                // Not using Empty(NumElements) to avoid clearing the hash memory since we'll rebuild it anyway
                // We need to make sure the relevant parts are cleared so ResizeAllocation can run safely and with minimal cost
                DestructItems(GetData(), this->NumElements);
                this->NumElements = 0;

                ResizeAllocation(Other.NumElements);

                this->NumElements = Other.NumElements;
                ConstructItems<ElementType>(GetData(), Other.GetData(), this->NumElements);

                Rehash();
            }
            return *this;
        }

        /** Move assignment */
        TCompactSet& operator=(TCompactSet&& Other) noexcept
        {
            if (this != &Other)
            {
                Empty();
                this->Elements.MoveToEmpty(Other.Elements);
                this->NumElements = Other.NumElements;
                this->MaxElements = Other.MaxElements;
                Other.NumElements = 0;
                Other.MaxElements = 0;
            }
            return *this;
        }

        /** Initializer list assignment */
        TCompactSet& operator=(std::initializer_list<ElementType> InitList)
        {
            Reset();
            Append(InitList);
            return *this;
        }

        // ====================================================================
        // Core Operations
        // ====================================================================

        /**
         * @brief Removes all elements and optionally reserves space
         * @param ExpectedNumElements Number of elements to reserve for
         */
        void Empty(i32 ExpectedNumElements = 0)
        {
            DestructItems(GetData(), this->NumElements);
            this->NumElements = 0;
            ResizeAllocation(ExpectedNumElements);
            if (this->MaxElements > 0)
            {
                GetHashTableView().Reset();
            }
        }

        /**
         * @brief Efficiently empties the set, preserving allocations
         */
        void Reset()
        {
            if (this->NumElements > 0)
            {
                DestructItems(GetData(), this->NumElements);
                this->NumElements = 0;
                GetHashTableView().Reset();
            }
        }

        /**
         * @brief Shrinks allocation to fit current element count
         */
        void Shrink()
        {
            if (this->NumElements != this->MaxElements)
            {
                if (ResizeAllocationPreserveData(this->NumElements))
                {
                    Rehash();
                }
            }
        }

        /**
         * @brief Preallocates memory for Number elements
         */
        void Reserve(i32 Number)
        {
            // Makes sense only when Number > Elements.Num() since we
            // do work only if that's the case
            if (static_cast<USizeType>(Number) > static_cast<USizeType>(this->MaxElements))
            {
                // Trap negative reserves
                if (Number < 0)
                {
                    OLO_CORE_ASSERT(false, "Invalid negative reserve: %d", Number);
                    return;
                }

                if (ResizeAllocationPreserveData(Number))
                {
                    Rehash();
                }
            }
        }

        // ====================================================================
        // Element Access
        // ====================================================================

        /**
         * @brief Gets pointer to element data
         */
        [[nodiscard]] OLO_FINLINE ElementType* GetData()
        {
            return reinterpret_cast<ElementType*>(this->Elements.GetAllocation());
        }

        [[nodiscard]] OLO_FINLINE const ElementType* GetData() const
        {
            return reinterpret_cast<const ElementType*>(this->Elements.GetAllocation());
        }

        /**
         * @brief Helper function to return the amount of memory allocated by this container
         * Only returns the size of allocations made directly by the container, not the elements themselves.
         * @return Number of bytes allocated by this container
         */
        [[nodiscard]] OLO_FINLINE sizet GetAllocatedSize() const
        {
            return Super::GetAllocatedSize(GetSetLayout());
        }

        /**
         * @brief Calculate the size of the hash table from the number of elements in the set
         * assuming the default number of hash elements
         */
        [[nodiscard]] OLO_FINLINE static constexpr sizet GetTotalMemoryRequiredInBytes(u32 NumElements)
        {
            return Super::GetTotalMemoryRequiredInBytes(NumElements, GetSetLayout());
        }

        /**
         * @brief Checks if an element ID is valid
         */
        [[nodiscard]] OLO_FINLINE bool IsValidId(FSetElementId Id) const
        {
            return Id.AsInteger() >= 0 && Id.AsInteger() < this->NumElements;
        }

        /**
         * @brief Accesses element by ID. Element must be valid (see @IsValidId).
         */
        [[nodiscard]] inline ElementType& operator[](FSetElementId Id)
        {
            RangeCheck(Id);
            return GetData()[Id.AsInteger()];
        }

        [[nodiscard]] inline const ElementType& operator[](FSetElementId Id) const
        {
            RangeCheck(Id);
            return GetData()[Id.AsInteger()];
        }

        /** Accesses the identified element's value. Element must be valid (see @IsValidId). */
        [[nodiscard]] inline ElementType& Get(FSetElementId Id)
        {
            RangeCheck(Id);
            return GetData()[Id.AsInteger()];
        }

        /** Accesses the identified element's value. Element must be valid (see @IsValidId). */
        [[nodiscard]] inline const ElementType& Get(FSetElementId Id) const
        {
            RangeCheck(Id);
            return GetData()[Id.AsInteger()];
        }

        // ====================================================================
        // Add Operations
        // ====================================================================

        /**
         * @brief Adds an element to the set
         * @param InElement Element to add
         * @param bIsAlreadyInSetPtr Optional output: true if element was already present
         * @return ID of the added/existing element
         */
        OLO_FINLINE FSetElementId Add(const ElementType& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return EmplaceByHashImpl(KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(InElement)),
                                     InElement, bIsAlreadyInSetPtr);
        }

        OLO_FINLINE FSetElementId Add(ElementType&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return EmplaceByHashImpl(KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(InElement)),
                                     MoveTemp(InElement), bIsAlreadyInSetPtr);
        }

        /**
         * @brief Adds element with precomputed hash
         */
        OLO_FINLINE FSetElementId AddByHash(u32 KeyHash, const ElementType& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return EmplaceByHashImpl(KeyHash, InElement, bIsAlreadyInSetPtr);
        }

        OLO_FINLINE FSetElementId AddByHash(u32 KeyHash, ElementType&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return EmplaceByHashImpl(KeyHash, MoveTemp(InElement), bIsAlreadyInSetPtr);
        }

        /**
         * @brief Constructs an element in-place
         * @param Arg The argument(s) to be forwarded to the set element's constructor
         * @param bIsAlreadyInSetPtr Optional output: true if element was already present
         * @return A handle to the element stored in the set
         */
        template<typename ArgType = ElementType>
        OLO_FINLINE FSetElementId Emplace(ArgType&& Arg, bool* bIsAlreadyInSetPtr = nullptr)
        {
            TPair<FSetElementId, bool> Result = Emplace(InPlace, Forward<ArgType>(Arg));

            if (bIsAlreadyInSetPtr)
            {
                *bIsAlreadyInSetPtr = Result.Value;
            }

            return Result.Key;
        }

        /**
         * @brief Adds an element to the set by constructing the ElementType in-place
         *
         * @param InPlace Tag to disambiguate in-place construction
         * @param InArgs Arguments forwarded to ElementType's constructor
         * @return Pair of (element id, whether an equivalent element already existed)
         */
        template<typename... ArgTypes>
        TPair<FSetElementId, bool> Emplace(EInPlace, ArgTypes&&... InArgs)
        {
            alignas(alignof(ElementType)) char StackElement[sizeof(ElementType)];
            ElementType& TempElement = *new (StackElement) ElementType(Forward<ArgTypes>(InArgs)...);

            const u32 KeyHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(TempElement));
            i32 ExistingIndex = INDEX_NONE;

            if constexpr (!KeyFuncs::bAllowDuplicateKeys)
            {
                ExistingIndex = FindIndexByHash(KeyHash, KeyFuncs::GetSetKey(TempElement));
            }

            const bool bAlreadyInSet = ExistingIndex != INDEX_NONE;

            if (!bAlreadyInSet)
            {
                ExistingIndex = this->NumElements;
                ElementType& NewElement = AddUninitialized(KeyHash);
                RelocateConstructItem<ElementType, ElementType>(&NewElement, &TempElement);
            }
            else
            {
                ElementType& ExistingElement = GetData()[ExistingIndex];
                MoveByRelocate(ExistingElement, TempElement);
            }

            return TPair<FSetElementId, bool>(FSetElementId::FromInteger(ExistingIndex), bAlreadyInSet);
        }

        /**
         * @brief Constructs an element in-place with precomputed hash
         * @param KeyHash A precomputed hash value, calculated in the same way as ElementType is hashed
         * @param Arg The argument(s) to be forwarded to the set element's constructor
         * @param bIsAlreadyInSetPtr Optional output: true if element was already present
         * @return A handle to the element stored in the set
         */
        template<typename ArgType = ElementType>
        OLO_FINLINE FSetElementId EmplaceByHash(u32 KeyHash, ArgType&& Arg, bool* bIsAlreadyInSetPtr = nullptr)
        {
            TPair<FSetElementId, bool> Result = EmplaceByHash(InPlace, KeyHash, Forward<ArgType>(Arg));

            if (bIsAlreadyInSetPtr)
            {
                *bIsAlreadyInSetPtr = Result.Value;
            }

            return Result.Key;
        }

        /**
         * @brief Constructs an element in-place with precomputed hash
         *
         * @param InPlace Tag to disambiguate in-place construction
         * @param KeyHash A precomputed hash value
         * @param InArgs Arguments forwarded to ElementType's constructor
         * @return Pair of (element id, whether an equivalent element already existed)
         *
         * @see Class documentation section on ByHash() functions
         */
        template<typename... ArgTypes>
        TPair<FSetElementId, bool> EmplaceByHash(EInPlace, u32 KeyHash, ArgTypes&&... InArgs)
        {
            alignas(alignof(ElementType)) char StackElement[sizeof(ElementType)];
            ElementType& TempElement = *new (StackElement) ElementType(Forward<ArgTypes>(InArgs)...);

            OLO_CORE_ASSERT(KeyHash == KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(TempElement)),
                            "Hash mismatch in EmplaceByHash(InPlace, ...)");

            i32 ExistingIndex = INDEX_NONE;

            if constexpr (!KeyFuncs::bAllowDuplicateKeys)
            {
                ExistingIndex = FindIndexByHash(KeyHash, KeyFuncs::GetSetKey(TempElement));
            }

            const bool bAlreadyInSet = ExistingIndex != INDEX_NONE;

            if (!bAlreadyInSet)
            {
                ExistingIndex = this->NumElements;
                ElementType& NewElement = AddUninitialized(KeyHash);
                RelocateConstructItem<ElementType, ElementType>(&NewElement, &TempElement);
            }
            else
            {
                ElementType& ExistingElement = GetData()[ExistingIndex];
                MoveByRelocate(ExistingElement, TempElement);
            }

            return TPair<FSetElementId, bool>(FSetElementId::FromInteger(ExistingIndex), bAlreadyInSet);
        }

        /**
         * @brief Appends elements from another container
         */
        template<typename RangeType>
        void Append(const RangeType& Range)
        {
            for (const auto& Element : Range)
            {
                Add(Element);
            }
        }

        template<typename RangeType>
        void Append(RangeType&& Range)
        {
            for (auto& Element : Range)
            {
                Add(MoveTemp(Element));
            }
        }

        void Append(std::initializer_list<ElementType> InitList)
        {
            Reserve(this->NumElements + static_cast<i32>(InitList.size()));
            for (const ElementType& Element : InitList)
            {
                Add(Element);
            }
        }

        // ====================================================================
        // Find Operations
        // ====================================================================

        /**
         * @brief Finds an element by key
         * @param Key The key to search for
         * @return A pointer to an element with the given key, or nullptr if not found
         */
        [[nodiscard]] inline ElementType* Find(KeyInitType Key)
        {
            SizeType ElementIndex = FindIndexByHash(KeyFuncs::GetKeyHash(Key), Key);
            if (ElementIndex != INDEX_NONE)
            {
                return GetData() + ElementIndex;
            }
            else
            {
                return nullptr;
            }
        }

        [[nodiscard]] OLO_FINLINE const ElementType* Find(KeyInitType Key) const
        {
            return const_cast<TCompactSet*>(this)->Find(Key);
        }

        /**
         * @brief Finds element ID by key
         * @param Key The key to search for
         * @return The id of the set element matching the given key, or the NULL id if none matches
         */
        [[nodiscard]] FSetElementId FindId(KeyInitType Key) const
        {
            return FSetElementId::FromInteger(FindIndexByHash(KeyFuncs::GetKeyHash(Key), Key));
        }

        /**
         * @brief Finds element ID with precomputed hash
         * @see Class documentation section on ByHash() functions
         * @return The element id that matches the key and hash or an invalid element id
         */
        template<typename ComparableKey>
        [[nodiscard]] FSetElementId FindIdByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            OLO_CORE_ASSERT(KeyHash == KeyFuncs::GetKeyHash(Key), "Hash mismatch in FindIdByHash");
            return FSetElementId::FromInteger(FindIndexByHash(KeyHash, Key));
        }

        /**
         * @brief Finds element with precomputed hash
         * @return Pointer to element, or nullptr if not found
         */
        template<typename ComparableKey>
        [[nodiscard]] ElementType* FindByHash(u32 KeyHash, const ComparableKey& Key)
        {
            if (this->NumElements == 0)
            {
                return nullptr;
            }

            const FConstCompactHashTableView HashTable = GetConstHashTableView();

            for (u32 Index = HashTable.GetFirst(KeyHash);
                 Index != INDEX_NONE;
                 Index = HashTable.GetNext(Index, this->NumElements))
            {
                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(GetData()[Index]), Key))
                {
                    return &GetData()[Index];
                }
            }

            return nullptr;
        }

        template<typename ComparableKey>
        [[nodiscard]] const ElementType* FindByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            return const_cast<TCompactSet*>(this)->FindByHash(KeyHash, Key);
        }

        /**
         * @brief Finds any element in the set and returns a pointer to it
         *
         * Callers should not depend on particular patterns in the behaviour of this function.
         *
         * @return A pointer to an arbitrary element, or nullptr if the container is empty
         */
        [[nodiscard]] ElementType* FindArbitraryElement()
        {
            return (this->NumElements > 0) ? GetData() : nullptr;
        }

        [[nodiscard]] const ElementType* FindArbitraryElement() const
        {
            return const_cast<TCompactSet*>(this)->FindArbitraryElement();
        }

        /**
         * @brief Checks if the element contains an element with the given key
         * @param Key The key to check for
         * @return True if the set contains an element with the given key
         */
        [[nodiscard]] OLO_FINLINE bool Contains(KeyInitType Key) const
        {
            return FindIndexByHash(KeyFuncs::GetKeyHash(Key), Key) != INDEX_NONE;
        }

        /**
         * @brief Checks if the element contains an element with the given key using precomputed hash
         * @see Class documentation section on ByHash() functions
         * @param KeyHash Precomputed hash of the key
         * @param Key Key to search for
         * @return True if the element exists
         */
        template<typename ComparableKey>
        [[nodiscard]] inline bool ContainsByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            OLO_CORE_ASSERT(KeyHash == KeyFuncs::GetKeyHash(Key), "Hash mismatch in ContainsByHash");
            return FindIndexByHash(KeyHash, Key) != INDEX_NONE;
        }

        /**
         * @brief Adds an element if not already present, returns reference to existing or new element
         * @param InElement Element to add
         * @param bIsAlreadyInSetPtr Optional output: true if element was already present
         * @return Reference to element in set
         */
        OLO_FINLINE ElementType& FindOrAdd(const ElementType& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return FindOrAddByHash(KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(InElement)), InElement, bIsAlreadyInSetPtr);
        }

        OLO_FINLINE ElementType& FindOrAdd(ElementType&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            return FindOrAddByHash(KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(InElement)), MoveTemp(InElement), bIsAlreadyInSetPtr);
        }

        /**
         * @brief FindOrAdd with precomputed hash
         * @see Class documentation section on ByHash() functions
         * @param KeyHash A precomputed hash value, calculated in the same way as ElementType is hashed
         * @param InElement Element to add to set
         * @param bIsAlreadyInSetPtr Optional output: true if element was already present
         * @return A reference to the element stored in the set
         */
        template<typename ElementReferenceType>
        ElementType& FindOrAddByHash(u32 KeyHash, ElementReferenceType&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            const SizeType ExistingIndex = FindIndexByHash(KeyHash, KeyFuncs::GetSetKey(InElement));
            const bool bIsAlreadyInSet = ExistingIndex != INDEX_NONE;
            if (bIsAlreadyInSetPtr)
            {
                *bIsAlreadyInSetPtr = bIsAlreadyInSet;
            }
            if (bIsAlreadyInSet)
            {
                return GetData()[ExistingIndex];
            }

            ElementType& NewElement = AddUninitialized(KeyHash);
            return *(new (static_cast<void*>(&NewElement)) ElementType(Forward<ElementReferenceType>(InElement)));
        }

        // ====================================================================
        // Remove Operations
        // ====================================================================

        /**
         * @brief Removes all elements from the set matching the specified key
         * @param Key The key to match elements against
         * @return The number of elements removed
         */
        i32 Remove(KeyInitType Key)
        {
            if (this->NumElements)
            {
                return RemoveImpl(KeyFuncs::GetKeyHash(Key), Key);
            }
            return 0;
        }

        /**
         * @brief Removes an element from the set
         * @param ElementId A pointer to the element in the set, as returned by Add or Find
         */
        void Remove(FSetElementId ElementId)
        {
            RemoveByIndex(ElementId.AsInteger());
        }

        /**
         * @brief Removes element by ID (swaps with last element)
         * @deprecated Use Remove(FSetElementId) instead. This is kept for backwards compatibility.
         */
        void RemoveById(FSetElementId Id)
        {
            Remove(Id);
        }

        /**
         * @brief Removes an element from the set while maintaining set order
         * @param ElementId A pointer to the element in the set, as returned by Add or Find
         */
        void RemoveStable(FSetElementId ElementId)
        {
            RemoveByIndex<true>(ElementId.AsInteger());
        }

        /**
         * @brief Removes all elements from the set matching the specified key while maintaining order
         * @param Key The key to match elements against
         * @return The number of elements removed
         */
        i32 RemoveStable(KeyInitType Key)
        {
            if (this->NumElements)
            {
                return RemoveImplStable(KeyFuncs::GetKeyHash(Key), Key);
            }
            return 0;
        }

        /**
         * @brief Removes element by hash and key
         * @return Number of elements removed
         */
        template<typename ComparableKey>
        i32 RemoveByHash(u32 KeyHash, const ComparableKey& Key)
        {
            OLO_CORE_ASSERT(KeyHash == KeyFuncs::GetKeyHash(Key), "Hash mismatch");
            if (this->NumElements)
            {
                return RemoveImpl(KeyHash, Key);
            }
            return 0;
        }

        // ====================================================================
        // Iteration
        // ====================================================================

        [[nodiscard]] TIterator CreateIterator()
        {
            return TIterator(*this);
        }

        [[nodiscard]] TConstIterator CreateConstIterator() const
        {
            return TConstIterator(*this);
        }

        // ====================================================================
        // Range-based For Support
        // ====================================================================

        /**
         * DO NOT USE DIRECTLY
         * STL-like iterators to enable range-based for loop support.
         * Uses raw pointers for maximum performance in release builds.
         */
        using TRangedForIterator = ElementType*;
        using TRangedForConstIterator = const ElementType*;

        [[nodiscard]] OLO_FINLINE ElementType* begin()
        {
            return GetData();
        }
        [[nodiscard]] OLO_FINLINE const ElementType* begin() const
        {
            return GetData();
        }
        [[nodiscard]] OLO_FINLINE ElementType* end()
        {
            return GetData() + this->NumElements;
        }
        [[nodiscard]] OLO_FINLINE const ElementType* end() const
        {
            return GetData() + this->NumElements;
        }

        // Sets are deliberately prevented from being hashed or compared, because this would hide
        // potentially major performance problems behind default operations.
        friend u32 GetTypeHash(const TCompactSet&) = delete;
        friend bool operator==(const TCompactSet&, const TCompactSet&) = delete;
        friend bool operator!=(const TCompactSet&, const TCompactSet&) = delete;

        // ====================================================================
        // Set Operations
        // ====================================================================

        /**
         * @brief Returns the union of this set with another (A OR B)
         */
        [[nodiscard]] TCompactSet Union(const TCompactSet& OtherSet) const
        {
            TCompactSet Result;
            Result.Reserve(this->NumElements + OtherSet.Num());

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
         * @brief Returns the intersection of this set with another (A AND B)
         */
        [[nodiscard]] TCompactSet Intersect(const TCompactSet& OtherSet) const
        {
            TCompactSet Result;

            // Iterate over the smaller set for efficiency
            if (this->NumElements <= OtherSet.Num())
            {
                for (const ElementType& Element : *this)
                {
                    if (OtherSet.Contains(KeyFuncs::GetSetKey(Element)))
                    {
                        Result.Add(Element);
                    }
                }
            }
            else
            {
                for (const ElementType& Element : OtherSet)
                {
                    if (Contains(KeyFuncs::GetSetKey(Element)))
                    {
                        Result.Add(Element);
                    }
                }
            }
            return Result;
        }

        /**
         * @brief Returns the difference (elements in this but not in other)
         */
        [[nodiscard]] TCompactSet Difference(const TCompactSet& OtherSet) const
        {
            TCompactSet Result;
            Result.Reserve(this->NumElements);

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
         * @brief Determine whether the specified set is entirely included within this set
         */
        [[nodiscard]] bool Includes(const TCompactSet& OtherSet) const
        {
            if (OtherSet.Num() > this->NumElements)
            {
                return false;
            }

            for (const ElementType& Element : OtherSet)
            {
                if (!Contains(KeyFuncs::GetSetKey(Element)))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Compacts the set (no-op for TCompactSet since it's always compact)
         * @note Provided for API compatibility with TSparseSet
         */
        void Compact()
        {
            // TCompactSet is always compact - no holes possible
            // This method exists for API compatibility with TSparseSet
        }

        /**
         * @brief Deprecated - use sparse array if this behavior is required
         * @note Keeping this here so TCompactSet can be swapped with TSet without changing code
         */
        void CompactStable()
        {
            OLO_CORE_ASSERT(false, "Compact sets are always compact so CompactStable will not do anything. "
                                   "If you hit this then you likely need to use a different pattern to maintain order, see RemoveStable");
        }

        /**
         * @brief Deprecated - unnecessary
         * @note Keeping this here so TCompactSet can be swapped with TSet without changing code
         */
        void SortFreeList()
        {
            // No-op for TCompactSet
        }

        /**
         * @brief Deprecated - default behavior now
         * @note Keeping this here so TCompactSet can be swapped with TSet without changing code
         */
        void Relax()
        {
            // No-op for TCompactSet
        }

        /**
         * @brief Sorts the set's elements using the provided comparison predicate
         */
        template<typename PredicateType>
        void Sort(const PredicateType& Predicate)
        {
            // Sort the elements using TArrayView (which uses Algo::Sort internally)
            TArrayView<ElementType>(GetData(), this->NumElements).Sort(Predicate);
            // Rehash since element positions changed
            Rehash();
        }

        /**
         * @brief Stable sorts the set's elements using the provided comparison predicate
         */
        template<typename PredicateType>
        void StableSort(const PredicateType& Predicate)
        {
            // Stable sort the elements using TArrayView
            TArrayView<ElementType>(GetData(), this->NumElements).StableSort(Predicate);
            // Rehash since element positions changed
            Rehash();
        }

        /**
         * @brief Returns an array copy of all elements
         */
        [[nodiscard]] TArray<ElementType> Array() const
        {
            TArray<ElementType> Result;
            Result.Reserve(this->NumElements);
            for (const ElementType& Element : *this)
            {
                Result.Add(Element);
            }
            return Result;
        }

        /**
         * @brief Returns a const TArrayView of the elements
         */
        [[nodiscard]] TArrayView<const ElementType> ArrayView() const
        {
            return TArrayView<const ElementType>(GetData(), this->NumElements);
        }

        // ====================================================================
        // Debugging & Memory Tracking
        // ====================================================================

        /**
         * @brief Count bytes for memory tracking via archive
         */
        void CountBytes(FArchive& Ar) const
        {
            Ar.CountBytes(this->NumElements * sizeof(ElementType), this->MaxElements * sizeof(ElementType));
            if (this->MaxElements > 0)
            {
                const FCompactSetLayout Layout = GetSetLayout();
                const sizet HashTableSize = this->GetAllocatedSize(Layout) - (this->MaxElements * Layout.Size);
                Ar.CountBytes(HashTableSize, HashTableSize);
            }
        }

        /**
         * @brief Checks array invariants: if array size is greater than or equal to zero
         * and less than or equal to the maximum.
         */
        OLO_FINLINE void CheckInvariants() const
        {
            OLO_CORE_ASSERT((this->NumElements >= 0) & (this->MaxElements >= this->NumElements),
                            "Set invariant violated: NumElements=%d, MaxElements=%d", this->NumElements, this->MaxElements);
        }

        /**
         * @brief Checks if index is in array range
         * @param Id Element ID to check
         */
        inline void RangeCheck(FSetElementId Id) const
        {
            CheckInvariants();
            OLO_CORE_ASSERT(IsValidId(Id), "Set index out of bounds: %d into a set of size %d",
                            Id.AsInteger(), this->NumElements);
        }

        // ====================================================================
        // Debug / Diagnostics
        // ====================================================================

        /**
         * @brief Describes the set's contents through an output device
         *
         * @note This method requires FOutputDevice to be fully defined.
         *       Currently stubbed until FOutputDevice is implemented.
         *       Future signature: void Dump(FOutputDevice& Ar) const
         */
        void Dump() const
        {
            // TODO: Implement when FOutputDevice is available
        }

        // ====================================================================
        // Memory Image Serialization (for cooked data)
        // ====================================================================

        /**
         * @brief Writes set to memory image for frozen data
         */
        void WriteMemoryImage(FMemoryImageWriter& Writer) const
        {
            if constexpr (TAllocatorTraits<Allocator>::SupportsFreezeMemoryImage && THasTypeLayout<InElementType>::Value)
            {
                if (this->NumElements > 0)
                {
                    const ElementType* Data = GetData();
                    OLO_CORE_ASSERT(Data != nullptr, "Data pointer is null");

                    FMemoryImageWriter ArrayWriter = Writer.WritePointer(StaticGetTypeLayoutDesc<ElementType>());
                    ArrayWriter.WriteAlignment(GetAlignment());

                    // Write active element data
                    ArrayWriter.WriteObjectArray(Data, StaticGetTypeLayoutDesc<ElementType>(), this->NumElements);
                    ArrayWriter.WritePaddingToSize(this->MaxElements * sizeof(ElementType));

                    // Write hash table data
                    const FCompactSetLayout Layout = GetSetLayout();
                    const HashCountType* HashTable = this->GetHashTableMemory(Layout);
                    const u8* HashTableDataEnd = reinterpret_cast<const u8*>(Data) +
                                                 Super::GetTotalMemoryRequiredInBytes(this->MaxElements, *HashTable, Layout);
                    ArrayWriter.WriteBytes(HashTable, HashTableDataEnd - reinterpret_cast<const u8*>(HashTable));

                    Writer.WriteBytes(this->NumElements);
                    Writer.WriteBytes(this->MaxElements);
                }
                else
                {
                    Writer.WriteBytes(TCompactSet());
                }
            }
            else
            {
                Writer.WriteBytes(TCompactSet());
            }
        }

        /**
         * @brief Copies unfrozen data from memory image
         */
        void CopyUnfrozen(const FMemoryUnfreezeContent& Context, void* Dst) const
        {
            ::new (Dst) TCompactSet();

            if constexpr (TAllocatorTraits<Allocator>::SupportsFreezeMemoryImage && THasTypeLayout<InElementType>::Value)
            {
                const FTypeLayoutDesc& ElementTypeDesc = StaticGetTypeLayoutDesc<ElementType>();
                TCompactSet* DstObject = static_cast<TCompactSet*>(Dst);

                DstObject->ResizeAllocation(this->MaxElements);
                DstObject->NumElements = this->NumElements;

                const ElementType* SrcData = GetData();
                ElementType* DstData = DstObject->GetData();

                for (i32 Index = 0; Index < this->NumElements; ++Index)
                {
                    Context.UnfreezeObject(SrcData + Index, ElementTypeDesc, DstData + Index);
                }

                const FCompactSetLayout Layout = GetSetLayout();
                const HashCountType* SrcHashTable = this->GetHashTableMemory(Layout);
                const u8* SrcHashTableEnd = reinterpret_cast<const u8*>(SrcData) +
                                            Super::GetTotalMemoryRequiredInBytes(this->MaxElements, *SrcHashTable, Layout);

                HashCountType* DstHashTable = const_cast<HashCountType*>(DstObject->GetHashTableMemory(Layout));
                std::memcpy(DstHashTable, SrcHashTable, SrcHashTableEnd - reinterpret_cast<const u8*>(SrcHashTable));
            }
        }

        /**
         * @brief Appends hash for memory image verification
         */
        static void AppendHash(const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
        {
            if constexpr (TAllocatorTraits<Allocator>::SupportsFreezeMemoryImage && THasTypeLayout<ElementType>::Value)
            {
                Freeze::AppendHash<ElementType>(StaticGetTypeLayoutDesc<ElementType>(), LayoutParams, Hasher);
            }
        }

        // ====================================================================
        // Serialization
        // ====================================================================

        /**
         * @brief Serializes the set to/from an archive
         */
        friend FArchive& operator<<(FArchive& Ar, TCompactSet& Set)
        {
            Set.CountBytes(Ar);

            i32 NumElements = Set.Num();
            Ar << NumElements;

            if (Ar.IsLoading())
            {
                // Destruct existing elements
                DestructItems(Set.GetData(), Set.NumElements);
                Set.NumElements = 0;
                Set.ResizeAllocation(NumElements);

                ElementType* Data = Set.GetData();
                for (i32 ElementIndex = 0; ElementIndex < NumElements; ++ElementIndex)
                {
                    Ar << *::new (static_cast<void*>(Data + ElementIndex)) ElementType;
                }

                Set.NumElements = NumElements;
                Set.Rehash();
            }
            else
            {
                for (ElementType& Element : Set)
                {
                    Ar << Element;
                }
            }
            return Ar;
        }

        /**
         * @brief Serializes the set to/from a structured archive
         */
        friend void operator<<(FStructuredArchive::FSlot& Slot, TCompactSet& Set)
        {
            i32 NumElements = Set.Num();
            FStructuredArchive::FArray Array = Slot.EnterArray(NumElements);

            if (Slot.GetUnderlyingArchive().IsLoading())
            {
                // Destruct existing elements
                DestructItems(Set.GetData(), Set.NumElements);
                Set.NumElements = 0;
                Set.ResizeAllocation(NumElements);

                ElementType* Data = Set.GetData();
                for (i32 ElementIndex = 0; ElementIndex < NumElements; ++ElementIndex)
                {
                    FStructuredArchive::FSlot ElementSlot = Array.EnterElement();
                    ElementSlot << *::new (static_cast<void*>(Data + ElementIndex)) ElementType;
                }

                Set.NumElements = NumElements;
                Set.Rehash();
            }
            else
            {
                for (ElementType& Element : Set)
                {
                    FStructuredArchive::FSlot ElementSlot = Array.EnterElement();
                    ElementSlot << Element;
                }
            }
        }

      private:
        // ====================================================================
        // Internal Helpers
        // ====================================================================

        [[nodiscard]] static constexpr FCompactSetLayout GetSetLayout()
        {
            return FCompactSetLayout{ sizeof(ElementType), static_cast<i32>(GetAlignment()) };
        }

        [[nodiscard]] static constexpr sizet GetAlignment()
        {
            return Max(alignof(ElementType), CompactHashTable::GetMemoryAlignment());
        }

        void ResizeAllocation(i32 NewMaxElements)
        {
            Super::ResizeAllocation(NewMaxElements, GetSetLayout());
        }

        [[nodiscard]] bool ResizeAllocationPreserveData(i32 NewMaxElements)
        {
            return Super::ResizeAllocationPreserveData(NewMaxElements, GetSetLayout());
        }

        [[nodiscard]] FCompactHashTableView GetHashTableView()
        {
            return Super::GetHashTableView(GetSetLayout());
        }

        [[nodiscard]] FConstCompactHashTableView GetConstHashTableView() const
        {
            return Super::GetConstHashTableView(GetSetLayout());
        }

        /** Recalculate the lookup table, used after bulk operations are performed */
        void Rehash()
        {
            if (this->MaxElements > 0)
            {
                const ElementType* ElementData = GetData();
                FCompactHashTableView HashTable = GetHashTableView();

                HashTable.Reset();

                for (i32 Index = 0; Index < this->NumElements; ++Index)
                {
                    HashTable.Add(Index, KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(ElementData[Index])));
                }
            }
        }

        /**
         * @brief Adds an uninitialized element and returns reference to it
         * @param KeyHash The precomputed hash of the key
         * @return Reference to the newly added uninitialized element
         */
        [[nodiscard]] ElementType& AddUninitialized(u32 KeyHash)
        {
            OLO_CORE_ASSERT(this->MaxElements >= 0, "Invalid MaxElements");
            if (this->NumElements == this->MaxElements)
            {
                Reserve(this->AllocatorCalculateSlackGrow(this->NumElements + 1, GetSetLayout()));
            }
            GetHashTableView().Add(this->NumElements, KeyHash);
            return GetData()[this->NumElements++];
        }

        /**
         * @brief Finds element index by hash and key
         * @param KeyHash Precomputed hash value
         * @param Key The key to search for
         * @return Index of the element, or INDEX_NONE if not found
         */
        template<typename ComparableKey>
        [[nodiscard]] i32 FindIndexByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            if (this->NumElements == 0)
            {
                return INDEX_NONE;
            }

            const FConstCompactHashTableView HashTable = GetConstHashTableView();
            const ElementType* Data = GetData();

            for (u32 Index = HashTable.GetFirst(KeyHash);
                 Index != INDEX_NONE;
                 Index = HashTable.GetNext(Index, this->NumElements))
            {
                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Data[Index]), Key))
                {
                    return static_cast<i32>(Index);
                }
            }

            return INDEX_NONE;
        }

        template<typename ElementArg>
        FSetElementId EmplaceByHashImpl(u32 KeyHash, ElementArg&& InElement, bool* bIsAlreadyInSetPtr)
        {
            // Grow if needed before we construct the element
            if (this->NumElements >= this->MaxElements)
            {
                const i32 NewMax = this->MaxElements == 0 ? 4 : this->MaxElements * 2;
                if (ResizeAllocationPreserveData(NewMax))
                {
                    Rehash();
                }
            }

            // Construct the element at the next available position (temporary)
            const i32 NewIndex = this->NumElements++;
            ElementType* Data = GetData();
            ElementType& NewElement = *new (&Data[NewIndex]) ElementType(Forward<ElementArg>(InElement));

            // Now check for duplicates using the constructed element
            if constexpr (!KeyFuncs::bAllowDuplicateKeys)
            {
                // Check if there's an existing element with the same key (excluding our new element)
                const FConstCompactHashTableView HashTable = GetConstHashTableView();

                for (u32 Index = HashTable.GetFirst(KeyHash);
                     Index != INDEX_NONE;
                     Index = HashTable.GetNext(Index, this->NumElements))
                {
                    // Skip the element we just added
                    if (static_cast<i32>(Index) == NewIndex)
                    {
                        continue;
                    }

                    if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Data[Index]), KeyFuncs::GetSetKey(NewElement)))
                    {
                        // Found duplicate - replace existing element with new one via relocation
                        ElementType& ExistingElement = Data[Index];
                        DestructItem(&ExistingElement);
                        RelocateConstructItems<ElementType>(&ExistingElement, &NewElement, 1);
                        --this->NumElements;

                        if (bIsAlreadyInSetPtr)
                        {
                            *bIsAlreadyInSetPtr = true;
                        }
                        return FSetElementId::FromInteger(Index);
                    }
                }
            }

            if (bIsAlreadyInSetPtr)
            {
                *bIsAlreadyInSetPtr = false;
            }

            // Add to hash table
            AddToHashTable(NewIndex, KeyHash);

            return FSetElementId::FromInteger(NewIndex);
        }

        void AddToHashTable(i32 Index, u32 KeyHash)
        {
            GetHashTableView().Add(Index, KeyHash);
        }

        void RemoveFromHashTable(i32 Index, u32 RemovedHash, i32 LastIndex, u32 LastHash)
        {
            GetHashTableView().Remove(Index, RemovedHash, LastIndex, LastHash);
        }

        void RemoveFromHashTableStable(i32 Index, u32 RemovedHash)
        {
            GetHashTableView().RemoveStable(Index, RemovedHash);
        }

        template<bool IsStable = false>
        void RemoveByIndex(const SizeType ElementIndex)
        {
            OLO_CORE_ASSERT(ElementIndex >= 0 && ElementIndex < this->NumElements,
                            "Invalid ElementIndex passed to TCompactSet::RemoveByIndex");
            RemoveByIndexAndHash<IsStable>(ElementIndex, KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(GetData()[ElementIndex])));
        }

        template<bool IsStable = false>
        void RemoveByIndexAndHash(const SizeType ElementIndex, const u32 KeyHash)
        {
            OLO_CORE_ASSERT(ElementIndex >= 0 && ElementIndex < this->NumElements,
                            "Invalid ElementIndex passed to TCompactSet::RemoveByIndex");

            ElementType* ElementsData = GetData();
            FCompactHashTableView HashTable = GetHashTableView();

            const SizeType LastElementIndex = this->NumElements - 1;
            if (ElementIndex == LastElementIndex)
            {
                HashTable.Remove(ElementIndex, KeyHash, ElementIndex, KeyHash);
                ElementsData[LastElementIndex].~ElementType();
            }
            else
            {
                if constexpr (IsStable)
                {
                    HashTable.RemoveStable(ElementIndex, KeyHash);

                    ElementsData[ElementIndex].~ElementType();
                    RelocateConstructItems<ElementType>(ElementsData + ElementIndex, ElementsData + ElementIndex + 1, LastElementIndex - ElementIndex);
                }
                else
                {
                    const u32 LastElementHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(ElementsData[LastElementIndex]));
                    HashTable.Remove(ElementIndex, KeyHash, LastElementIndex, LastElementHash);

                    MoveByRelocate(ElementsData[ElementIndex], ElementsData[LastElementIndex]);
                }
            }

            --this->NumElements;
        }

        template<typename ComparableKey, bool IsStable = false>
        i32 RemoveImpl(u32 KeyHash, const ComparableKey& Key)
        {
            OLO_CORE_ASSERT(this->NumElements > 0, "Cannot remove from empty set");
            i32 NumRemovedElements = 0;

            const ElementType* ElementsData = GetData();
            FCompactHashTableView HashTable = GetHashTableView();

            SizeType LastElementIndex = INDEX_NONE;
            SizeType ElementIndex = HashTable.GetFirst(KeyHash);

            while (ElementIndex != INDEX_NONE)
            {
                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(ElementsData[ElementIndex]), Key))
                {
                    RemoveByIndexAndHash<IsStable>(ElementIndex, KeyHash);
                    NumRemovedElements++;

                    if constexpr (!KeyFuncs::bAllowDuplicateKeys)
                    {
                        // If the hash disallows duplicate keys, we're done removing after the first matched key.
                        break;
                    }
                    else
                    {
                        if (LastElementIndex == INDEX_NONE)
                        {
                            ElementIndex = HashTable.GetFirst(KeyHash);
                        }
                        else
                        {
                            if (LastElementIndex == this->NumElements) // Would have been remapped to ElementIndex
                            {
                                LastElementIndex = ElementIndex;
                            }

                            ElementIndex = HashTable.GetNext(LastElementIndex, this->NumElements);
                        }
                    }
                }
                else
                {
                    LastElementIndex = ElementIndex;
                    ElementIndex = HashTable.GetNext(LastElementIndex, this->NumElements);
                }
            }

            return NumRemovedElements;
        }

        template<typename ComparableKey>
        OLO_FINLINE i32 RemoveImplStable(u32 KeyHash, const ComparableKey& Key)
        {
            return RemoveImpl<ComparableKey, true>(KeyHash, Key);
        }
    };

    // ============================================================================
    // TIsCompactSet Specialization
    // ============================================================================

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsCompactSet<TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    // ============================================================================
    // TIsTSet Specializations (cv-qualified)
    // ============================================================================

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsTSet<TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsTSet<const TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsTSet<volatile TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsTSet<const volatile TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    // ============================================================================
    // Deduction Guides
    // ============================================================================

    // TODO: Re-enable when TElementType_T trait is ported
    // template <typename RangeType>
    // TCompactSet(RangeType&&) -> TCompactSet<TElementType_T<RangeType>>;

    // ============================================================================
    // Memory Image Support Functions
    // ============================================================================

    namespace Freeze
    {
        template<typename ElementType, typename KeyFuncs, typename Allocator>
        void IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer,
                                       const TCompactSet<ElementType, KeyFuncs, Allocator>& Object,
                                       const FTypeLayoutDesc&)
        {
            Object.WriteMemoryImage(Writer);
        }

        template<typename ElementType, typename KeyFuncs, typename Allocator>
        u32 IntrinsicUnfrozenCopy(const FMemoryUnfreezeContent& Context,
                                  const TCompactSet<ElementType, KeyFuncs, Allocator>& Object,
                                  void* OutDst)
        {
            Object.CopyUnfrozen(Context, OutDst);
            return sizeof(Object);
        }

        template<typename ElementType, typename KeyFuncs, typename Allocator>
        u32 IntrinsicAppendHash(const TCompactSet<ElementType, KeyFuncs, Allocator>* DummyObject,
                                const FTypeLayoutDesc& TypeDesc,
                                const FPlatformTypeLayoutParameters& LayoutParams,
                                FSHA1& Hasher)
        {
            TCompactSet<ElementType, KeyFuncs, Allocator>::AppendHash(LayoutParams, Hasher);
            return DefaultAppendHash(TypeDesc, LayoutParams, Hasher);
        }
    } // namespace Freeze

    // ============================================================================
    // Legacy Comparison Functions
    // ============================================================================

    /**
     * @brief Legacy comparison that also tests element order
     */
    template<typename ElementType, typename KeyFuncs, typename Allocator>
    [[nodiscard]] bool LegacyCompareEqual(const TCompactSet<ElementType, KeyFuncs, Allocator>& A,
                                          const TCompactSet<ElementType, KeyFuncs, Allocator>& B)
    {
        return A.Num() == B.Num() && CompareItems(A.GetData(), B.GetData(), A.Num());
    }

    template<typename ElementType, typename KeyFuncs, typename Allocator>
    [[nodiscard]] bool LegacyCompareNotEqual(const TCompactSet<ElementType, KeyFuncs, Allocator>& A,
                                             const TCompactSet<ElementType, KeyFuncs, Allocator>& B)
    {
        return !LegacyCompareEqual(A, B);
    }

} // namespace OloEngine
