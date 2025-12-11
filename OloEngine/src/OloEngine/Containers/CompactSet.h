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
#include "OloEngine/Memory/MemoryOps.h"
#include <algorithm>
#include <initializer_list>
#include <type_traits>

namespace OloEngine
{
    // Forward declarations
    template <typename KeyType, typename ValueType>
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
    template <
        typename InElementType,
        typename KeyFuncs = DefaultKeyFuncs<InElementType>,
        typename Allocator = FDefaultAllocator
    >
    class TCompactSet
        : public TCompactSetBase<Allocator>
    {
    public:
        using ElementType = InElementType;
        using KeyFuncsType = KeyFuncs;
        using AllocatorType = Allocator;

    private:
        using Super = TCompactSetBase<Allocator>;
        using typename Super::SizeType;
        using typename Super::HashCountType;
        using USizeType = std::make_unsigned_t<SizeType>;
        using KeyInitType = typename KeyFuncs::KeyInitType;
        using ElementInitType = typename KeyFuncs::ElementInitType;

    public:
        // Bring base class public members into scope
        using Super::Num;
        using Super::Max;
        using Super::GetMaxIndex;
        using Super::IsEmpty;
        // ====================================================================
        // Iterator Types
        // ====================================================================

        /**
         * @class TBaseIterator
         * @brief Base iterator for the compact set
         */
        template <bool bConst>
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
                : Set(InSet)
                , Index(StartIndex)
#if !OLO_BUILD_SHIPPING
                , InitialNum(InSet.Num())
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
                --Super::Index;  // Compensate since last element was moved here
#if !OLO_BUILD_SHIPPING
                --Super::InitialNum;
#endif
            }
        };

        /**
         * @class TBaseKeyIterator
         * @brief Base iterator for finding elements by key (handles hash collisions)
         */
        template <bool bConst>
        class TBaseKeyIterator
        {
        private:
            using SetType = std::conditional_t<bConst, const TCompactSet, TCompactSet>;
            using ItElementType = std::conditional_t<bConst, const ElementType, ElementType>;
            using ReferenceOrValueType = typename std::add_lvalue_reference_t<typename std::add_const_t<typename KeyFuncs::KeyType>>;

        public:
            using KeyArgumentType = KeyInitType;

            /** Initialization constructor */
            [[nodiscard]] inline TBaseKeyIterator(SetType& InSet, KeyArgumentType InKey)
                : Set(InSet)
                , Key(InKey)
                , HashTable(InSet.Num() ? InSet.GetConstHashTableView() : FConstCompactHashTableView())
                , Index(INDEX_NONE)
                , NextIndex(InSet.Num() ? HashTable.GetFirst(KeyFuncs::GetKeyHash(Key)) : INDEX_NONE)
#if !OLO_BUILD_SHIPPING
                , InitialNum(InSet.Num())
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
            KeyArgumentType Key;
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
            Empty();
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
        template <typename ViewElementType>
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
        template <typename OtherAllocator>
        [[nodiscard]] TCompactSet(TCompactSet<ElementType, KeyFuncs, OtherAllocator>&& Other)
        {
            Append(MoveTemp(Other));
        }

        /** Constructor for copying elements from a TCompactSet with a different allocator */
        template <typename OtherAllocator>
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
                Empty(Other.Num());
                Append(Other);
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
            if (static_cast<USizeType>(Number) > static_cast<USizeType>(this->MaxElements))
            {
                OLO_CORE_ASSERT(Number >= 0, "Negative reserve");
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
         * @brief Checks if an element ID is valid
         */
        [[nodiscard]] OLO_FINLINE bool IsValidId(FSetElementId Id) const
        {
            return Id.AsInteger() >= 0 && Id.AsInteger() < this->NumElements;
        }

        /**
         * @brief Accesses element by ID
         */
        [[nodiscard]] inline ElementType& operator[](FSetElementId Id)
        {
            OLO_CORE_ASSERT(IsValidId(Id), "Invalid set element ID");
            return GetData()[Id.AsInteger()];
        }

        [[nodiscard]] inline const ElementType& operator[](FSetElementId Id) const
        {
            OLO_CORE_ASSERT(IsValidId(Id), "Invalid set element ID");
            return GetData()[Id.AsInteger()];
        }

        /** Accesses the identified element's value. Element must be valid (see @IsValidId). */
        [[nodiscard]] OLO_FINLINE ElementType& Get(FSetElementId Id)
        {
            OLO_CORE_ASSERT(IsValidId(Id), "Invalid set element ID");
            return GetData()[Id.AsInteger()];
        }

        /** Accesses the identified element's value. Element must be valid (see @IsValidId). */
        [[nodiscard]] OLO_FINLINE const ElementType& Get(FSetElementId Id) const
        {
            OLO_CORE_ASSERT(IsValidId(Id), "Invalid set element ID");
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
         */
        template <typename... ArgsType>
        FSetElementId Emplace(ArgsType&&... Args)
        {
            ElementType TempElement(Forward<ArgsType>(Args)...);
            return Add(MoveTemp(TempElement));
        }

        /**
         * @brief Adds an element to the set by constructing the ElementType in-place
         * 
         * @param InPlace Tag to disambiguate in-place construction
         * @param InArgs Arguments forwarded to ElementType's constructor
         * @return Pair of (element id, whether an equivalent element already existed)
         */
        template <typename... ArgTypes>
        TPair<FSetElementId, bool> Emplace(EInPlace, ArgTypes&&... InArgs)
        {
            alignas(alignof(ElementType)) char StackElement[sizeof(ElementType)];
            ElementType& TempElement = *new (StackElement) ElementType(Forward<ArgTypes>(InArgs)...);
            bool bIsAlreadyInSet = false;
            
            const u32 KeyHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(TempElement));
            FSetElementId Id = EmplaceByHashImpl(KeyHash, MoveTemp(TempElement), &bIsAlreadyInSet);
            TempElement.~ElementType();
            
            return TPair<FSetElementId, bool>(Id, bIsAlreadyInSet);
        }

        /**
         * @brief Constructs an element in-place with precomputed hash (no duplicate check output)
         */
        template <typename ElementArg>
        FSetElementId EmplaceByHash(u32 KeyHash, ElementArg&& InElement)
        {
            return EmplaceByHashImpl(KeyHash, Forward<ElementArg>(InElement), nullptr);
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
        template <typename... ArgTypes>
        TPair<FSetElementId, bool> EmplaceByHash(EInPlace, u32 KeyHash, ArgTypes&&... InArgs)
        {
            alignas(alignof(ElementType)) char StackElement[sizeof(ElementType)];
            ElementType& TempElement = *new (StackElement) ElementType(Forward<ArgTypes>(InArgs)...);
            bool bIsAlreadyInSet = false;
            
            OLO_CORE_ASSERT(KeyHash == KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(TempElement)), 
                "Hash mismatch in EmplaceByHash(InPlace, ...)");
            
            FSetElementId Id = EmplaceByHashImpl(KeyHash, MoveTemp(TempElement), &bIsAlreadyInSet);
            TempElement.~ElementType();
            
            return TPair<FSetElementId, bool>(Id, bIsAlreadyInSet);
        }

        /**
         * @brief Appends elements from another container
         */
        template <typename RangeType>
        void Append(const RangeType& Range)
        {
            for (const auto& Element : Range)
            {
                Add(Element);
            }
        }

        template <typename RangeType>
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
         * @return Pointer to element, or nullptr if not found
         */
        [[nodiscard]] ElementType* Find(KeyInitType Key)
        {
            FSetElementId Id = FindId(Key);
            return Id.IsValidId() ? &GetData()[Id.AsInteger()] : nullptr;
        }

        [[nodiscard]] const ElementType* Find(KeyInitType Key) const
        {
            return const_cast<TCompactSet*>(this)->Find(Key);
        }

        /**
         * @brief Finds element ID by key
         */
        [[nodiscard]] FSetElementId FindId(KeyInitType Key) const
        {
            if (this->NumElements == 0)
            {
                return FSetElementId();
            }
            return FindIdByHash(KeyFuncs::GetKeyHash(Key), Key);
        }

        /**
         * @brief Finds element ID with precomputed hash
         */
        [[nodiscard]] FSetElementId FindIdByHash(u32 KeyHash, KeyInitType Key) const
        {
            if (this->NumElements == 0)
            {
                return FSetElementId();
            }

            const FConstCompactHashTableView HashTable = GetConstHashTableView();
            
            for (u32 Index = HashTable.GetFirst(KeyHash); 
                 Index != INDEX_NONE; 
                 Index = HashTable.GetNext(Index, this->NumElements))
            {
                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(GetData()[Index]), Key))
                {
                    return FSetElementId::FromInteger(Index);
                }
            }

            return FSetElementId();
        }

        /**
         * @brief Finds element with precomputed hash
         * @return Pointer to element, or nullptr if not found
         */
        template <typename ComparableKey>
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

        template <typename ComparableKey>
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
         * @brief Checks if element exists
         */
        [[nodiscard]] bool Contains(KeyInitType Key) const
        {
            return FindId(Key).IsValidId();
        }

        /**
         * @brief Checks if element exists using precomputed hash
         * 
         * @param KeyHash Precomputed hash of the key
         * @param Key Key to search for
         * @return True if the element exists
         * 
         * @see Class documentation section on ByHash() functions
         */
        template <typename ComparableKey>
        [[nodiscard]] bool ContainsByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            OLO_CORE_ASSERT(KeyHash == KeyFuncs::GetKeyHash(Key), "Hash mismatch in ContainsByHash");
            return FindIdByHash(KeyHash, Key).IsValidId();
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
         */
        template <typename ElementArg>
        ElementType& FindOrAddByHash(u32 KeyHash, ElementArg&& InElement, bool* bIsAlreadyInSetPtr = nullptr)
        {
            const i32 ExistingIndex = FindIdByHash(KeyHash, KeyFuncs::GetSetKey(InElement)).AsInteger();
            const bool bIsAlreadyInSet = ExistingIndex >= 0;
            if (bIsAlreadyInSetPtr)
            {
                *bIsAlreadyInSetPtr = bIsAlreadyInSet;
            }
            if (bIsAlreadyInSet)
            {
                return GetData()[ExistingIndex];
            }

            FSetElementId NewId = EmplaceByHashImpl(KeyHash, Forward<ElementArg>(InElement), nullptr);
            return GetData()[NewId.AsInteger()];
        }

        // ====================================================================
        // Remove Operations
        // ====================================================================

        /**
         * @brief Removes an element by key
         * @return Number of elements removed (0 or 1)
         */
        i32 Remove(KeyInitType Key)
        {
            FSetElementId Id = FindId(Key);
            if (Id.IsValidId())
            {
                RemoveById(Id);
                return 1;
            }
            return 0;
        }

        /**
         * @brief Removes element by ID
         * @note Provided for API compatibility with TSparseSet
         */
        void Remove(FSetElementId Id)
        {
            RemoveById(Id);
        }

        /**
         * @brief Removes element by ID (swaps with last element)
         */
        void RemoveById(FSetElementId Id)
        {
            OLO_CORE_ASSERT(IsValidId(Id), "Invalid ID");

            const i32 Index = Id.AsInteger();
            const i32 LastIndex = this->NumElements - 1;

            ElementType* Data = GetData();
            const u32 RemovedHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Data[Index]));
            const u32 LastHash = (Index != LastIndex) ? 
                KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Data[LastIndex])) : 0;

            // Remove from hash table
            RemoveFromHashTable(Index, RemovedHash, LastIndex, LastHash);

            // Destruct removed element
            DestructItem(&Data[Index]);

            // Move last element to fill hole (if not already last)
            if (Index != LastIndex)
            {
                RelocateConstructItems<ElementType>(&Data[Index], &Data[LastIndex], 1);
            }

            --this->NumElements;
        }

        /**
         * @brief Removes element by ID while preserving order (expensive)
         */
        void RemoveStable(FSetElementId Id)
        {
            RemoveByIndexStable(Id.AsInteger());
        }

        /**
         * @brief Removes element by key while preserving order (expensive)
         * @return Number of elements removed (0 or 1)
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
        template <typename ComparableKey>
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

        // Range-for support (return iterators for compatibility with TSparseSet)
        [[nodiscard]] TIterator begin() { return TIterator(*this, 0); }
        [[nodiscard]] TConstIterator begin() const { return TConstIterator(*this, 0); }
        [[nodiscard]] TIterator end() { return TIterator(*this, this->NumElements); }
        [[nodiscard]] TConstIterator end() const { return TConstIterator(*this, this->NumElements); }

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
         * @brief Compacts the set while preserving order (no-op for TCompactSet)
         * @note Provided for API compatibility with TSparseSet
         */
        void CompactStable()
        {
            // TCompactSet is always compact - no holes possible
        }

        /**
         * @brief Sorts the set's elements using the provided comparison predicate
         */
        template <typename PredicateType>
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
        template <typename PredicateType>
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
         * @brief Validates internal invariants (debug only)
         */
        void CheckInvariants() const
        {
#if !OLO_BUILD_SHIPPING
            OLO_CORE_ASSERT(this->NumElements >= 0, "NumElements cannot be negative");
            OLO_CORE_ASSERT(this->MaxElements >= 0 || this->MaxElements == INDEX_NONE, "Invalid MaxElements");
            OLO_CORE_ASSERT(this->NumElements <= this->MaxElements || this->MaxElements == INDEX_NONE, "NumElements exceeds MaxElements");
            
            // Verify hash table integrity
            if (this->NumElements > 0)
            {
                const ElementType* Data = GetData();
                for (i32 i = 0; i < this->NumElements; ++i)
                {
                    const u32 Hash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Data[i]));
                    FSetElementId FoundId = FindIdByHash(Hash, KeyFuncs::GetSetKey(Data[i]));
                    OLO_CORE_ASSERT(FoundId.IsValidId(), "Element not found in hash table");
                }
            }
#endif
        }

        /**
         * @brief Range check an index (asserts in debug)
         */
        OLO_FINLINE void RangeCheck(i32 Index) const
        {
            OLO_CORE_ASSERT(Index >= 0 && Index < this->NumElements, 
                "Set index out of range: %d (Num=%d)", Index, this->NumElements);
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
            ::new(Dst) TCompactSet();

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

        template <typename ElementArg>
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

        void RemoveByIndex(i32 ElementIndex)
        {
            OLO_CORE_ASSERT(ElementIndex >= 0 && ElementIndex < this->NumElements, "Invalid ElementIndex");
            RemoveByIndexAndHash(ElementIndex, KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(GetData()[ElementIndex])));
        }

        void RemoveByIndexAndHash(i32 ElementIndex, u32 KeyHash)
        {
            OLO_CORE_ASSERT(ElementIndex >= 0 && ElementIndex < this->NumElements, "Invalid ElementIndex");

            ElementType* Data = GetData();
            const i32 LastIndex = this->NumElements - 1;

            if (ElementIndex == LastIndex)
            {
                RemoveFromHashTable(ElementIndex, KeyHash, ElementIndex, KeyHash);
                DestructItem(&Data[LastIndex]);
            }
            else
            {
                const u32 LastHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Data[LastIndex]));
                RemoveFromHashTable(ElementIndex, KeyHash, LastIndex, LastHash);
                DestructItem(&Data[ElementIndex]);
                RelocateConstructItems<ElementType>(&Data[ElementIndex], &Data[LastIndex], 1);
            }

            --this->NumElements;
        }

        void RemoveByIndexStable(i32 ElementIndex)
        {
            OLO_CORE_ASSERT(ElementIndex >= 0 && ElementIndex < this->NumElements, "Invalid ElementIndex");

            ElementType* Data = GetData();
            const u32 KeyHash = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Data[ElementIndex]));
            const i32 LastIndex = this->NumElements - 1;

            RemoveFromHashTableStable(ElementIndex, KeyHash);
            DestructItem(&Data[ElementIndex]);

            // Shift all elements after the removed one
            if (ElementIndex != LastIndex)
            {
                RelocateConstructItems<ElementType>(&Data[ElementIndex], &Data[ElementIndex + 1], LastIndex - ElementIndex);
            }

            --this->NumElements;
        }

        template <typename ComparableKey>
        i32 RemoveImpl(u32 KeyHash, const ComparableKey& Key)
        {
            OLO_CORE_ASSERT(this->NumElements > 0, "Cannot remove from empty set");
            i32 NumRemovedElements = 0;

            ElementType* Data = GetData();
            FCompactHashTableView HashTable = GetHashTableView();

            i32 LastElementIndex = INDEX_NONE;
            i32 ElementIndex = static_cast<i32>(HashTable.GetFirst(KeyHash));

            while (ElementIndex != INDEX_NONE)
            {
                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Data[ElementIndex]), Key))
                {
                    RemoveByIndexAndHash(ElementIndex, KeyHash);
                    NumRemovedElements++;

                    if constexpr (!KeyFuncs::bAllowDuplicateKeys)
                    {
                        break;
                    }
                    else
                    {
                        // Re-fetch since removal may have changed indices
                        if (LastElementIndex == INDEX_NONE)
                        {
                            ElementIndex = static_cast<i32>(HashTable.GetFirst(KeyHash));
                        }
                        else
                        {
                            if (LastElementIndex == this->NumElements)
                            {
                                LastElementIndex = ElementIndex;
                            }
                            ElementIndex = static_cast<i32>(HashTable.GetNext(LastElementIndex));
                        }
                    }
                }
                else
                {
                    LastElementIndex = ElementIndex;
                    ElementIndex = static_cast<i32>(HashTable.GetNext(LastElementIndex));
                }
            }

            return NumRemovedElements;
        }

        template <typename ComparableKey>
        i32 RemoveImplStable(u32 KeyHash, const ComparableKey& Key)
        {
            OLO_CORE_ASSERT(this->NumElements > 0, "Cannot remove from empty set");
            i32 NumRemovedElements = 0;

            // For stable removal, find and remove one at a time
            while (true)
            {
                FSetElementId Id = FindIdByHash(KeyHash, Key);
                if (!Id.IsValidId())
                {
                    break;
                }

                RemoveByIndexStable(Id.AsInteger());
                NumRemovedElements++;

                if constexpr (!KeyFuncs::bAllowDuplicateKeys)
                {
                    break;
                }
            }

            return NumRemovedElements;
        }
    };

    // ============================================================================
    // TIsCompactSet Specialization
    // ============================================================================

    template <typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsCompactSet<TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    // ============================================================================
    // TIsTSet Specializations (cv-qualified)
    // ============================================================================

    template <typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsTSet<TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template <typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsTSet<const TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template <typename ElementType, typename KeyFuncs, typename Allocator>
    struct TIsTSet<volatile TCompactSet<ElementType, KeyFuncs, Allocator>>
    {
        static constexpr bool Value = true;
    };

    template <typename ElementType, typename KeyFuncs, typename Allocator>
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
        template <typename ElementType, typename KeyFuncs, typename Allocator>
        void IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer, 
                                        const TCompactSet<ElementType, KeyFuncs, Allocator>& Object, 
                                        const FTypeLayoutDesc&)
        {
            Object.WriteMemoryImage(Writer);
        }

        template <typename ElementType, typename KeyFuncs, typename Allocator>
        u32 IntrinsicUnfrozenCopy(const FMemoryUnfreezeContent& Context, 
                                   const TCompactSet<ElementType, KeyFuncs, Allocator>& Object, 
                                   void* OutDst)
        {
            Object.CopyUnfrozen(Context, OutDst);
            return sizeof(Object);
        }

        template <typename ElementType, typename KeyFuncs, typename Allocator>
        u32 IntrinsicAppendHash(const TCompactSet<ElementType, KeyFuncs, Allocator>* DummyObject, 
                                 const FTypeLayoutDesc& TypeDesc, 
                                 const FPlatformTypeLayoutParameters& LayoutParams, 
                                 FSHA1& Hasher)
        {
            TCompactSet<ElementType, KeyFuncs, Allocator>::AppendHash(LayoutParams, Hasher);
            return DefaultAppendHash(TypeDesc, LayoutParams, Hasher);
        }
    }

    // ============================================================================
    // Legacy Comparison Functions
    // ============================================================================

    /**
     * @brief Legacy comparison that also tests element order
     */
    template <typename ElementType, typename KeyFuncs, typename Allocator>
    [[nodiscard]] bool LegacyCompareEqual(const TCompactSet<ElementType, KeyFuncs, Allocator>& A, 
                                           const TCompactSet<ElementType, KeyFuncs, Allocator>& B)
    {
        return A.Num() == B.Num() && CompareItems(A.GetData(), B.GetData(), A.Num());
    }

    template <typename ElementType, typename KeyFuncs, typename Allocator>
    [[nodiscard]] bool LegacyCompareNotEqual(const TCompactSet<ElementType, KeyFuncs, Allocator>& A, 
                                              const TCompactSet<ElementType, KeyFuncs, Allocator>& B)
    {
        return !LegacyCompareEqual(A, B);
    }

} // namespace OloEngine
