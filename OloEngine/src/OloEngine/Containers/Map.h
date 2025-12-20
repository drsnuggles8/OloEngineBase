#pragma once

/**
 * @file Map.h
 * @brief Hash-based map container with O(1) average operations
 *
 * Provides a hash-based map implementation using TSet for key-value storage:
 * - O(1) average case for add, remove, and find operations
 * - Customizable key functions for different comparison and hashing strategies
 * - Support for heterogeneous lookup with ByHash() functions
 * - Iteration maintains insertion order (via underlying sparse array)
 *
 * The ByHash() functions are somewhat dangerous but particularly useful in two scenarios:
 * -- Heterogeneous lookup to avoid creating expensive keys like FString when looking up by const TCHAR*.
 *    You must ensure the hash is calculated in the same way as ElementType is hashed.
 *    If possible put both ComparableKey and ElementType hash functions next to each other in the same header
 *    to avoid bugs when the ElementType hash function is changed.
 * -- Reducing contention around hash tables protected by a lock. It is often important to incur
 *    the cache misses of reading key data and doing the hashing *before* acquiring the lock.
 *
 * Ported from Unreal Engine's Containers/Map.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Algo/Reverse.h"
#include "OloEngine/Containers/Set.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Templates/Sorting.h"
#include "OloEngine/Templates/Tuple.h"
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace OloEngine
{
    // Forward declarations for constraints
    template<typename KeyInitType, typename ValueInitType>
    class TPairInitializer;

    template<typename KeyInitType>
    class TKeyInitializer;

    // Helper to detect TPairInitializer
    template<typename T>
    struct TIsPairInitializer : std::false_type
    {
    };

    template<typename K, typename V>
    struct TIsPairInitializer<TPairInitializer<K, V>> : std::true_type
    {
    };

    // Helper to detect TKeyInitializer
    template<typename T>
    struct TIsKeyInitializer : std::false_type
    {
    };

    template<typename K>
    struct TIsKeyInitializer<TKeyInitializer<K>> : std::true_type
    {
    };

    // Combined check for any initializer type
    template<typename T>
    inline constexpr bool TIsAnyInitializer = TIsPairInitializer<std::remove_cvref_t<T>>::value || TIsKeyInitializer<std::remove_cvref_t<T>>::value;

    // ============================================================================
    // TPairInitializer - For initializing pairs during Add operations
    // ============================================================================

    /**
     * @class TPairInitializer
     * @brief Initializer type for pairs during map Add operations
     */
    template<typename KeyInitType, typename ValueInitType>
    class TPairInitializer
    {
      public:
        std::conditional_t<std::is_rvalue_reference_v<KeyInitType>, KeyInitType&, KeyInitType> Key;
        std::conditional_t<std::is_rvalue_reference_v<ValueInitType>, ValueInitType&, ValueInitType> Value;

        [[nodiscard]] inline TPairInitializer(KeyInitType InKey, ValueInitType InValue)
            : Key(InKey), Value(InValue)
        {
        }

        template<typename KeyType, typename ValueType>
        [[nodiscard]] inline TPairInitializer(const TPair<KeyType, ValueType>& Pair)
            : Key(Pair.Key), Value(Pair.Value)
        {
        }

        template<typename KeyType, typename ValueType>
        [[nodiscard]] operator TPair<KeyType, ValueType>() const
        {
            return TPair<KeyType, ValueType>(static_cast<KeyInitType>(Key), static_cast<ValueInitType>(Value));
        }
    };

    // ============================================================================
    // TKeyInitializer - For initializing pairs with only a key
    // ============================================================================

    /**
     * @class TKeyInitializer
     * @brief Initializer type for pairs when only adding a key (value is default-constructed)
     */
    template<typename KeyInitType>
    class TKeyInitializer
    {
      public:
        std::conditional_t<std::is_rvalue_reference_v<KeyInitType>, KeyInitType&, KeyInitType> Key;

        [[nodiscard]] OLO_FINLINE explicit TKeyInitializer(KeyInitType InKey)
            : Key(InKey)
        {
        }

        template<typename KeyType, typename ValueType>
        [[nodiscard]] operator TPair<KeyType, ValueType>() const
        {
            return TPair<KeyType, ValueType>(static_cast<KeyInitType>(Key), ValueType());
        }
    };

    // ============================================================================
    // TDefaultMapKeyFuncs - Default key functions for TMap
    // ============================================================================

    /**
     * @struct TDefaultMapKeyFuncs
     * @brief Default key functions for TMap - extracts key from TPair
     *
     * Uses TTypeTraits from TypeTraits.h for ConstPointerType and ConstInitType.
     */
    template<typename KeyType, typename ValueType, bool bInAllowDuplicateKeys>
    struct TDefaultMapKeyFuncs : BaseKeyFuncs<TPair<KeyType, ValueType>, KeyType, bInAllowDuplicateKeys>
    {
        using KeyInitType = typename TTypeTraits<KeyType>::ConstPointerType;
        using ElementInitType = const TPairInitializer<typename TTypeTraits<KeyType>::ConstInitType, typename TTypeTraits<ValueType>::ConstInitType>&;

        [[nodiscard]] static OLO_FINLINE KeyInitType GetSetKey(ElementInitType Element)
        {
            return Element.Key;
        }

        [[nodiscard]] static OLO_FINLINE bool Matches(KeyInitType A, KeyInitType B)
        {
            return A == B;
        }

        template<typename ComparableKey>
        [[nodiscard]] static OLO_FINLINE bool Matches(KeyInitType A, ComparableKey B)
        {
            return A == B;
        }

        [[nodiscard]] static OLO_FINLINE u32 GetKeyHash(KeyInitType Key)
        {
            return GetTypeHash(Key);
        }

        template<typename ComparableKey>
        [[nodiscard]] static OLO_FINLINE u32 GetKeyHash(ComparableKey Key)
        {
            return GetTypeHash(Key);
        }
    };

    /**
     * @struct TDefaultMapHashableKeyFuncs
     * @brief Default key functions with hashability check
     *
     * Ensures the key type has a GetTypeHash() overload at compile time.
     */
    template<typename KeyType, typename ValueType, bool bInAllowDuplicateKeys>
    struct TDefaultMapHashableKeyFuncs : TDefaultMapKeyFuncs<KeyType, ValueType, bInAllowDuplicateKeys>
    {
        // Check that the key type is actually hashable
        // If this line fails to compile then your key doesn't have a GetTypeHash() overload.
        using HashabilityCheck = decltype(GetTypeHash(std::declval<const KeyType>()));
    };

    // ============================================================================
    // TIsTMap Trait
    // ============================================================================

    template<typename T>
    struct TIsTMap
    {
        static constexpr bool Value = false;
    };

    // ============================================================================
    // TMapBase - Base class for TMap
    // ============================================================================

    /**
     * @class TMapBase
     * @brief Base class for TMap providing core functionality
     *
     * Implemented using a TSet of key-value pairs with custom KeyFuncs,
     * providing O(1) addition, removal, and finding.
     *
     * @tparam KeyType     The key type
     * @tparam ValueType   The value type
     * @tparam SetAllocator  Allocator policy for the underlying set
     * @tparam KeyFuncs    Functions for getting keys and computing hashes
     */
    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    class TMapBase
    {
        template<typename OtherKeyType, typename OtherValueType, typename OtherSetAllocator, typename OtherKeyFuncs>
        friend class TMapBase;

      public:
        using KeyConstPointerType = typename TTypeTraits<KeyType>::ConstPointerType;
        using KeyInitType = typename TTypeTraits<KeyType>::ConstInitType;
        using ValueInitType = typename TTypeTraits<ValueType>::ConstInitType;
        using ElementType = TPair<KeyType, ValueType>;

      protected:
        [[nodiscard]] constexpr TMapBase() = default;

        [[nodiscard]] explicit consteval TMapBase(EConstEval)
            : Pairs(ConstEval)
        {
        }

        [[nodiscard]] TMapBase(TMapBase&&) = default;
        [[nodiscard]] TMapBase(const TMapBase&) = default;
        TMapBase& operator=(TMapBase&&) = default;
        TMapBase& operator=(const TMapBase&) = default;

        /** Constructor for moving elements from a TMap with a different SetAllocator */
        template<typename OtherSetAllocator>
        [[nodiscard]] TMapBase(TMapBase<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& Other)
            : Pairs(MoveTemp(Other.Pairs))
        {
        }

        /** Constructor for copying elements from a TMap with a different SetAllocator */
        template<typename OtherSetAllocator>
        [[nodiscard]] TMapBase(const TMapBase<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& Other)
            : Pairs(Other.Pairs)
        {
        }

        ~TMapBase()
        {
            // Warn if used with non-trivially-relocatable types
            static_assert(TIsTriviallyRelocatable_V<KeyType> && TIsTriviallyRelocatable_V<ValueType>,
                          "TMapBase can only be used with trivially relocatable types");
        }

        // ========================================================================
        // Intrusive TOptional<TMapBase> state
        // ========================================================================
        static constexpr bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TMapBase;

        [[nodiscard]] explicit TMapBase(FIntrusiveUnsetOptionalState Tag)
            : Pairs(Tag)
        {
        }

        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState Tag) const
        {
            return Pairs == Tag;
        }

        /** Assignment operator for moving elements from a TMap with a different SetAllocator */
        template<typename OtherSetAllocator>
        TMapBase& operator=(TMapBase<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& Other)
        {
            Pairs = MoveTemp(Other.Pairs);
            return *this;
        }

        /** Assignment operator for copying elements from a TMap with a different SetAllocator */
        template<typename OtherSetAllocator>
        TMapBase& operator=(const TMapBase<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& Other)
        {
            Pairs = Other.Pairs;
            return *this;
        }

      public:
        // ========================================================================
        // Size / Capacity
        // ========================================================================

        /** @return true if the map is empty */
        [[nodiscard]] bool IsEmpty() const
        {
            return Pairs.IsEmpty();
        }

        /** @return the number of elements */
        [[nodiscard]] OLO_FINLINE i32 Num() const
        {
            return Pairs.Num();
        }

        /** @return the number of elements the map can hold before reallocation */
        [[nodiscard]] OLO_FINLINE i32 Max() const
        {
            return Pairs.Max();
        }

        /** @return the non-inclusive maximum index of elements in the map */
        [[nodiscard]] OLO_FINLINE i32 GetMaxIndex() const
        {
            return Pairs.GetMaxIndex();
        }

        /** @return the amount of memory allocated by this container */
        [[nodiscard]] OLO_FINLINE sizet GetAllocatedSize() const
        {
            return Pairs.GetAllocatedSize();
        }

        /**
         * Track the container's memory use through an archive.
         * @param Ar  The archive to use.
         */
        OLO_FINLINE void CountBytes(FArchive& Ar) const
        {
            Pairs.CountBytes(Ar);
        }

        /**
         * Compare this map with another for equality. Does not make any assumptions about Key order.
         * NOTE: this might be a candidate for operator== but it was decided to make it an explicit function
         *  since it can potentially be quite slow.
         *
         * @param Other The other map to compare against
         * @returns True if both this and Other contain the same keys with values that compare ==
         */
        [[nodiscard]] bool OrderIndependentCompareEqual(const TMapBase& Other) const
        {
            // First check counts (they should be the same obviously)
            if (Num() != Other.Num())
            {
                return false;
            }

            // Since we know the counts are the same, we can just iterate one map and check for existence in the other
            for (typename ElementSetType::TConstIterator It(Pairs); It; ++It)
            {
                const ValueType* BVal = Other.Find(It->Key);
                if (BVal == nullptr)
                {
                    return false;
                }
                if (!(*BVal == It->Value))
                {
                    return false;
                }
            }

            // All fields in A match B and A and B's counts are the same (so there can be no fields in B not in A)
            return true;
        }

        /**
         * Get the unique keys contained within this map.
         *
         * @param OutKeys  Upon return, contains the set of unique keys in this map.
         * @return The number of unique keys in the map.
         */
        template<typename Allocator>
        i32 GetKeys(TArray<KeyType, Allocator>& OutKeys) const
        {
            OutKeys.Reset();

            TSet<KeyType> VisitedKeys;
            VisitedKeys.Reserve(Num());

            // Presize the array if we know there are supposed to be no duplicate keys
            if constexpr (!KeyFuncs::bAllowDuplicateKeys)
            {
                OutKeys.Reserve(Num());
            }

            for (typename ElementSetType::TConstIterator It(Pairs); It; ++It)
            {
                // Even if bAllowDuplicateKeys is false, we still want to filter for duplicate
                // keys due to maps with keys that can be invalidated (UObjects, TWeakObj, etc.)
                if (!VisitedKeys.Contains(It->Key))
                {
                    OutKeys.Add(It->Key);
                    VisitedKeys.Add(It->Key);
                }
            }

            return OutKeys.Num();
        }

        /**
         * Get the unique keys contained within this map (into a set).
         *
         * @param OutKeys  Upon return, contains the set of unique keys in this map.
         * @return The number of unique keys in the map.
         */
        template<typename InSetKeyFuncs, typename InSetAllocator>
        i32 GetKeys(TSet<KeyType, InSetKeyFuncs, InSetAllocator>& OutKeys) const
        {
            OutKeys.Reset();

            // Presize the set if we know there are supposed to be no duplicate keys
            if constexpr (!KeyFuncs::bAllowDuplicateKeys)
            {
                OutKeys.Reserve(Num());
            }

            for (typename ElementSetType::TConstIterator It(Pairs); It; ++It)
            {
                OutKeys.Add(It->Key);
            }

            return OutKeys.Num();
        }

        // ========================================================================
        // Element Access
        // ========================================================================

        /**
         * Checks whether an element id is valid.
         * @param Id  The element id to check
         * @return true if the id refers to a valid element
         */
        [[nodiscard]] OLO_FINLINE bool IsValidId(FSetElementId Id) const
        {
            return Pairs.IsValidId(Id);
        }

        /** Return a mapped pair by internal identifier */
        [[nodiscard]] OLO_FINLINE ElementType& Get(FSetElementId Id)
        {
            return Pairs[Id];
        }

        /** Return a mapped pair by internal identifier (const) */
        [[nodiscard]] OLO_FINLINE const ElementType& Get(FSetElementId Id) const
        {
            return Pairs[Id];
        }

        // ========================================================================
        // Modification
        // ========================================================================

        /**
         * @brief Remove all elements
         * @param ExpectedNumElements  Number of elements expected to be added
         */
        OLO_FINLINE void Empty(i32 ExpectedNumElements = 0)
        {
            Pairs.Empty(ExpectedNumElements);
        }

        /** Empty the map but keep allocations */
        OLO_FINLINE void Reset()
        {
            Pairs.Reset();
        }

        /** Shrink pair storage to avoid slack */
        OLO_FINLINE void Shrink()
        {
            Pairs.Shrink();
        }

        /** Compact pairs into a contiguous range */
        OLO_FINLINE void Compact()
        {
            Pairs.Compact();
        }

        /** Compact pairs while preserving iteration order */
        OLO_FINLINE void CompactStable()
        {
            Pairs.CompactStable();
        }

        /** Preallocate memory for Number elements */
        OLO_FINLINE void Reserve(i32 Number)
        {
            Pairs.Reserve(Number);
        }

        // ========================================================================
        // Add / Emplace
        // ========================================================================

        /**
         * @brief Set the value associated with a key
         * @param InKey    The key to associate the value with
         * @param InValue  The value to associate with the key
         * @return Reference to the value in the map
         */
        OLO_FINLINE ValueType& Add(const KeyType& InKey, const ValueType& InValue)
        {
            return Emplace(InKey, InValue);
        }

        OLO_FINLINE ValueType& Add(const KeyType& InKey, ValueType&& InValue)
        {
            return Emplace(InKey, MoveTemp(InValue));
        }

        OLO_FINLINE ValueType& Add(KeyType&& InKey, const ValueType& InValue)
        {
            return Emplace(MoveTemp(InKey), InValue);
        }

        OLO_FINLINE ValueType& Add(KeyType&& InKey, ValueType&& InValue)
        {
            return Emplace(MoveTemp(InKey), MoveTemp(InValue));
        }

        /**
         * @brief Set a default value associated with a key
         * @param InKey  The key to associate a default value with
         * @return Reference to the value in the map
         */
        OLO_FINLINE ValueType& Add(const KeyType& InKey)
        {
            return Emplace(InKey);
        }

        OLO_FINLINE ValueType& Add(KeyType&& InKey)
        {
            return Emplace(MoveTemp(InKey));
        }

        /** Add with precomputed hash */
        OLO_FINLINE ValueType& AddByHash(u32 KeyHash, const KeyType& InKey, const ValueType& InValue)
        {
            return EmplaceByHash(KeyHash, InKey, InValue);
        }

        OLO_FINLINE ValueType& AddByHash(u32 KeyHash, const KeyType& InKey, ValueType&& InValue)
        {
            return EmplaceByHash(KeyHash, InKey, MoveTemp(InValue));
        }

        OLO_FINLINE ValueType& AddByHash(u32 KeyHash, KeyType&& InKey, const ValueType& InValue)
        {
            return EmplaceByHash(KeyHash, MoveTemp(InKey), InValue);
        }

        OLO_FINLINE ValueType& AddByHash(u32 KeyHash, KeyType&& InKey, ValueType&& InValue)
        {
            return EmplaceByHash(KeyHash, MoveTemp(InKey), MoveTemp(InValue));
        }

        OLO_FINLINE ValueType& AddByHash(u32 KeyHash, const KeyType& InKey)
        {
            return EmplaceByHash(KeyHash, InKey);
        }

        OLO_FINLINE ValueType& AddByHash(u32 KeyHash, KeyType&& InKey)
        {
            return EmplaceByHash(KeyHash, MoveTemp(InKey));
        }

        /**
         * @brief Construct a key-value pair in place
         * @param InKey    Key value
         * @param InValue  Value to associate with key
         * @return Reference to the value in the map
         */
        template<typename InitKeyType = KeyType, typename InitValueType = ValueType>
        ValueType& Emplace(InitKeyType&& InKey, InitValueType&& InValue)
        {
            const FSetElementId PairId = Pairs.Emplace(TPairInitializer<InitKeyType&&, InitValueType&&>(Forward<InitKeyType>(InKey), Forward<InitValueType>(InValue)));
            return Pairs[PairId].Value;
        }

        /** Emplace with precomputed hash */
        template<typename InitKeyType = KeyType, typename InitValueType = ValueType>
        ValueType& EmplaceByHash(u32 KeyHash, InitKeyType&& InKey, InitValueType&& InValue)
        {
            const FSetElementId PairId = Pairs.EmplaceByHash(KeyHash, TPairInitializer<InitKeyType&&, InitValueType&&>(Forward<InitKeyType>(InKey), Forward<InitValueType>(InValue)));
            return Pairs[PairId].Value;
        }

        /**
         * @brief Construct a key with default value in place
         * @param InKey  Key value
         * @return Reference to the value in the map
         */
        template<typename InitKeyType = KeyType>
        ValueType& Emplace(InitKeyType&& InKey)
        {
            const FSetElementId PairId = Pairs.Emplace(TKeyInitializer<InitKeyType&&>(Forward<InitKeyType>(InKey)));
            return Pairs[PairId].Value;
        }

        /** Emplace key-only with precomputed hash */
        template<typename InitKeyType = KeyType>
        ValueType& EmplaceByHash(u32 KeyHash, InitKeyType&& InKey)
        {
            const FSetElementId PairId = Pairs.EmplaceByHash(KeyHash, TKeyInitializer<InitKeyType&&>(Forward<InitKeyType>(InKey)));
            return Pairs[PairId].Value;
        }

        // ========================================================================
        // Remove
        // ========================================================================

        /**
         * @brief Remove all value associations for a key
         * @param InKey  The key to remove
         * @return Number of values removed
         */
        inline i32 Remove(KeyConstPointerType InKey)
        {
            return Pairs.Remove(InKey);
        }

        /**
         * @brief Remove all value associations for a key while preserving order
         * @param InKey  The key to remove
         * @return Number of values removed
         */
        inline i32 RemoveStable(KeyConstPointerType InKey)
        {
            return Pairs.RemoveStable(InKey);
        }

        /** Remove with precomputed hash */
        template<typename ComparableKey>
        inline i32 RemoveByHash(u32 KeyHash, const ComparableKey& Key)
        {
            return Pairs.RemoveByHash(KeyHash, Key);
        }

        /** Remove element by id */
        OLO_FINLINE void Remove(FSetElementId Id)
        {
            Pairs.Remove(Id);
        }

        // ========================================================================
        // Find / Contains
        // ========================================================================

        /**
         * @brief Find the value associated with a key
         * @param Key  The key to search for
         * @return Pointer to value or nullptr if not found
         */
        [[nodiscard]] inline ValueType* Find(KeyConstPointerType Key)
        {
            if (auto* Pair = Pairs.Find(Key))
            {
                return &Pair->Value;
            }
            return nullptr;
        }

        [[nodiscard]] OLO_FINLINE const ValueType* Find(KeyConstPointerType Key) const
        {
            return const_cast<TMapBase*>(this)->Find(Key);
        }

        /** Find with precomputed hash */
        template<typename ComparableKey>
        [[nodiscard]] inline ValueType* FindByHash(u32 KeyHash, const ComparableKey& Key)
        {
            if (auto* Pair = Pairs.FindByHash(KeyHash, Key))
            {
                return &Pair->Value;
            }
            return nullptr;
        }

        template<typename ComparableKey>
        [[nodiscard]] OLO_FINLINE const ValueType* FindByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            return const_cast<TMapBase*>(this)->FindByHash(KeyHash, Key);
        }

        /**
         * @brief Find value by hash, assert if not found
         * @param KeyHash  Precomputed hash of the key
         * @param Key      The key to search for
         * @return Reference to the value
         */
        template<typename ComparableKey>
        [[nodiscard]] inline ValueType& FindByHashChecked(u32 KeyHash, const ComparableKey& Key)
        {
            auto* Pair = Pairs.FindByHash(KeyHash, Key);
            OLO_CORE_ASSERT(Pair != nullptr, "Key not found in map");
            return Pair->Value;
        }

        template<typename ComparableKey>
        [[nodiscard]] OLO_FINLINE const ValueType& FindByHashChecked(u32 KeyHash, const ComparableKey& Key) const
        {
            return const_cast<TMapBase*>(this)->FindByHashChecked(KeyHash, Key);
        }

        /**
         * @brief Find the id of the pair with the given key
         * @param Key  The key to search for
         * @return Element id or invalid id if not found
         */
        OLO_FINLINE FSetElementId FindId(KeyInitType Key) const
        {
            return Pairs.FindId(Key);
        }

        /** Find id with precomputed hash */
        template<typename ComparableKey>
        OLO_FINLINE FSetElementId FindIdByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            return Pairs.FindIdByHash(KeyHash, Key);
        }

      private:
        [[nodiscard]] OLO_FINLINE static u32 HashKey(const KeyType& Key)
        {
            return KeyFuncs::GetKeyHash(Key);
        }

        template<typename InitKeyType>
        [[nodiscard]] ValueType& FindOrAddImpl(u32 KeyHash, InitKeyType&& Key)
        {
            if (auto* Pair = Pairs.FindByHash(KeyHash, Key))
            {
                return Pair->Value;
            }
            return AddByHash(KeyHash, Forward<InitKeyType>(Key));
        }

        template<typename InitKeyType, typename InitValueType>
        [[nodiscard]] ValueType& FindOrAddImpl(u32 KeyHash, InitKeyType&& Key, InitValueType&& Value)
        {
            if (auto* Pair = Pairs.FindByHash(KeyHash, Key))
            {
                return Pair->Value;
            }
            return AddByHash(KeyHash, Forward<InitKeyType>(Key), Forward<InitValueType>(Value));
        }

      public:
        /**
         * @brief Find the value associated with a key, or add a default value if not found
         * @param Key  The key to search for
         * @return Reference to the value
         */
        OLO_FINLINE ValueType& FindOrAdd(const KeyType& Key)
        {
            return FindOrAddImpl(HashKey(Key), Key);
        }

        OLO_FINLINE ValueType& FindOrAdd(KeyType&& Key)
        {
            return FindOrAddImpl(HashKey(Key), MoveTemp(Key));
        }

        /** FindOrAdd with precomputed hash */
        OLO_FINLINE ValueType& FindOrAddByHash(u32 KeyHash, const KeyType& Key)
        {
            return FindOrAddImpl(KeyHash, Key);
        }

        OLO_FINLINE ValueType& FindOrAddByHash(u32 KeyHash, KeyType&& Key)
        {
            return FindOrAddImpl(KeyHash, MoveTemp(Key));
        }

        /**
         * @brief Find the value associated with a key, or add with specified value if not found
         * @param Key    The key to search for
         * @param Value  The value to add if key not found
         * @return Reference to the value
         */
        OLO_FINLINE ValueType& FindOrAdd(const KeyType& Key, const ValueType& Value)
        {
            return FindOrAddImpl(HashKey(Key), Key, Value);
        }

        OLO_FINLINE ValueType& FindOrAdd(const KeyType& Key, ValueType&& Value)
        {
            return FindOrAddImpl(HashKey(Key), Key, MoveTemp(Value));
        }

        OLO_FINLINE ValueType& FindOrAdd(KeyType&& Key, const ValueType& Value)
        {
            return FindOrAddImpl(HashKey(Key), MoveTemp(Key), Value);
        }

        OLO_FINLINE ValueType& FindOrAdd(KeyType&& Key, ValueType&& Value)
        {
            return FindOrAddImpl(HashKey(Key), MoveTemp(Key), MoveTemp(Value));
        }

        /** FindOrAdd with precomputed hash */
        OLO_FINLINE ValueType& FindOrAddByHash(u32 KeyHash, const KeyType& Key, const ValueType& Value)
        {
            return FindOrAddImpl(KeyHash, Key, Value);
        }

        OLO_FINLINE ValueType& FindOrAddByHash(u32 KeyHash, const KeyType& Key, ValueType&& Value)
        {
            return FindOrAddImpl(KeyHash, Key, MoveTemp(Value));
        }

        OLO_FINLINE ValueType& FindOrAddByHash(u32 KeyHash, KeyType&& Key, const ValueType& Value)
        {
            return FindOrAddImpl(KeyHash, MoveTemp(Key), Value);
        }

        OLO_FINLINE ValueType& FindOrAddByHash(u32 KeyHash, KeyType&& Key, ValueType&& Value)
        {
            return FindOrAddImpl(KeyHash, MoveTemp(Key), MoveTemp(Value));
        }

        /**
         * @brief Find reference to value, assert if not found
         * @param Key  The key to search for
         * @return Reference to the value
         */
        [[nodiscard]] inline const ValueType& FindChecked(KeyConstPointerType Key) const
        {
            const auto* Pair = Pairs.Find(Key);
            OLO_CORE_ASSERT(Pair != nullptr, "Key not found in map");
            return Pair->Value;
        }

        [[nodiscard]] inline ValueType& FindChecked(KeyConstPointerType Key)
        {
            auto* Pair = Pairs.Find(Key);
            OLO_CORE_ASSERT(Pair != nullptr, "Key not found in map");
            return Pair->Value;
        }

        /**
         * @brief Find value or return default
         * @param Key  The key to search for
         * @return Value if found, or default-constructed value
         */
        [[nodiscard]] inline ValueType FindRef(KeyConstPointerType Key) const
        {
            if (const auto* Pair = Pairs.Find(Key))
            {
                return Pair->Value;
            }
            return ValueType();
        }

        /**
         * @brief Find value or return specified default
         * @param Key          The key to search for
         * @param DefaultValue The fallback value
         * @return Value if found, or DefaultValue
         */
        [[nodiscard]] inline ValueType FindRef(KeyConstPointerType Key, ValueType DefaultValue) const
        {
            if (const auto* Pair = Pairs.Find(Key))
            {
                return Pair->Value;
            }
            return DefaultValue;
        }

        /**
         * @brief Find the key associated with a value (linear search)
         * @param Value  The value to search for
         * @return Pointer to key or nullptr if not found
         */
        [[nodiscard]] const KeyType* FindKey(ValueInitType Value) const
        {
            for (auto It = Pairs.CreateConstIterator(); It; ++It)
            {
                if ((*It).Value == Value)
                {
                    return &(*It).Key;
                }
            }
            return nullptr;
        }

        /**
         * @brief Find any pair in the map
         * @return Pointer to an arbitrary pair, or nullptr if empty
         */
        [[nodiscard]] ElementType* FindArbitraryElement()
        {
            return Pairs.FindArbitraryElement();
        }

        [[nodiscard]] const ElementType* FindArbitraryElement() const
        {
            return const_cast<TMapBase*>(this)->FindArbitraryElement();
        }

        /**
         * @brief Check if map contains the specified key
         * @param Key  The key to check for
         * @return true if the map contains the key
         */
        [[nodiscard]] OLO_FINLINE bool Contains(KeyConstPointerType Key) const
        {
            return Pairs.Contains(Key);
        }

        /** Contains with precomputed hash */
        template<typename ComparableKey>
        [[nodiscard]] OLO_FINLINE bool ContainsByHash(u32 KeyHash, const ComparableKey& Key) const
        {
            return Pairs.FindIdByHash(KeyHash, Key).IsValidId();
        }

        /** Copy the key/value pairs in this map into an array. */
        [[nodiscard]] TArray<ElementType> Array() const
        {
            return Pairs.Array();
        }

        /**
         * Filters the elements in the map based on a predicate functor.
         *
         * @param Pred  The functor to apply to each element.
         * @returns TMap with the same type as this object which contains
         *          the subset of elements for which the functor returns true.
         */
        template<typename Predicate, typename MapType = TMapBase>
        [[nodiscard]] auto FilterByPredicate(Predicate Pred) const
        {
            // This will be specialized in TMap to return the proper type
            TArray<ElementType> FilterResults;
            FilterResults.Reserve(Pairs.Num());
            for (const ElementType& Pair : Pairs)
            {
                if (Pred(Pair))
                {
                    FilterResults.Add(Pair);
                }
            }
            return FilterResults;
        }

        /**
         * Generate an array from the keys in this map.
         *
         * @param OutArray  Will contain the collection of keys.
         */
        template<typename Allocator>
        void GenerateKeyArray(TArray<KeyType, Allocator>& OutArray) const
        {
            OutArray.Empty(Pairs.Num());
            for (typename ElementSetType::TConstIterator PairIt(Pairs); PairIt; ++PairIt)
            {
                OutArray.Add(PairIt->Key);
            }
        }

        /**
         * Generate an array from the values in this map.
         *
         * @param OutArray  Will contain the collection of values.
         */
        template<typename Allocator>
        void GenerateValueArray(TArray<ValueType, Allocator>& OutArray) const
        {
            OutArray.Empty(Pairs.Num());
            for (typename ElementSetType::TConstIterator PairIt(Pairs); PairIt; ++PairIt)
            {
                OutArray.Add(PairIt->Value);
            }
        }

        // ========================================================================
        // Iterators
        // ========================================================================

      protected:
        using ElementSetType = TSet<ElementType, KeyFuncs, SetAllocator>;

        /** Base iterator class */
        template<bool bConst>
        class TBaseIterator
        {
          public:
            using PairItType = std::conditional_t<
                bConst,
                typename ElementSetType::TConstIterator,
                typename ElementSetType::TIterator>;

          private:
            using ItKeyType = std::conditional_t<bConst, const KeyType, KeyType>;
            using ItValueType = std::conditional_t<bConst, const ValueType, ValueType>;
            using PairType = std::conditional_t<bConst, const typename ElementSetType::ElementType, typename ElementSetType::ElementType>;

          public:
            [[nodiscard]] OLO_FINLINE TBaseIterator(const PairItType& InElementIt)
                : PairIt(InElementIt)
            {
            }

            OLO_FINLINE TBaseIterator& operator++()
            {
                ++PairIt;
                return *this;
            }

            [[nodiscard]] OLO_FINLINE explicit operator bool() const
            {
                return static_cast<bool>(PairIt);
            }

            [[nodiscard]] OLO_FINLINE bool operator!() const
            {
                return !static_cast<bool>(*this);
            }

            [[nodiscard]] OLO_FINLINE bool operator==(const TBaseIterator& Rhs) const
            {
                return PairIt == Rhs.PairIt;
            }

            [[nodiscard]] OLO_FINLINE bool operator!=(const TBaseIterator& Rhs) const
            {
                return PairIt != Rhs.PairIt;
            }

            [[nodiscard]] OLO_FINLINE ItKeyType& Key() const
            {
                return PairIt->Key;
            }

            [[nodiscard]] OLO_FINLINE ItValueType& Value() const
            {
                return PairIt->Value;
            }

            [[nodiscard]] OLO_FINLINE FSetElementId GetId() const
            {
                return PairIt.GetId();
            }

            [[nodiscard]] OLO_FINLINE PairType& operator*() const
            {
                return *PairIt;
            }

            [[nodiscard]] OLO_FINLINE PairType* operator->() const
            {
                return &*PairIt;
            }

          protected:
            PairItType PairIt;
        };

        /** The base type of iterators that iterate over the values associated with a specified key. */
        template<bool bConst>
        class TBaseKeyIterator
        {
          private:
            using SetItType = std::conditional_t<bConst, typename ElementSetType::TConstKeyIterator, typename ElementSetType::TKeyIterator>;
            using ItKeyType = std::conditional_t<bConst, const KeyType, KeyType>;
            using ItValueType = std::conditional_t<bConst, const ValueType, ValueType>;

          public:
            /** Initialization constructor. */
            [[nodiscard]] OLO_FINLINE TBaseKeyIterator(const SetItType& InSetIt)
                : SetIt(InSetIt)
            {
            }

            OLO_FINLINE TBaseKeyIterator& operator++()
            {
                ++SetIt;
                return *this;
            }

            /** Conversion to "bool" returning true if the iterator is valid. */
            [[nodiscard]] OLO_FINLINE explicit operator bool() const
            {
                return static_cast<bool>(SetIt);
            }

            /** Inverse of the "bool" operator */
            [[nodiscard]] OLO_FINLINE bool operator!() const
            {
                return !static_cast<bool>(*this);
            }

            [[nodiscard]] OLO_FINLINE FSetElementId GetId() const
            {
                return SetIt.GetId();
            }

            [[nodiscard]] OLO_FINLINE ItKeyType& Key() const
            {
                return SetIt->Key;
            }

            [[nodiscard]] OLO_FINLINE ItValueType& Value() const
            {
                return SetIt->Value;
            }

            [[nodiscard]] OLO_FINLINE decltype(auto) operator*() const
            {
                return SetIt.operator*();
            }

            [[nodiscard]] OLO_FINLINE decltype(auto) operator->() const
            {
                return SetIt.operator->();
            }

          protected:
            SetItType SetIt;
        };

        /** A set of the key-value pairs in the map */
        ElementSetType Pairs;

      public:
        /** Map iterator */
        class TIterator : public TBaseIterator<false>
        {
          public:
            [[nodiscard]] inline TIterator(TMapBase& InMap, bool bInRequiresRehashOnRemoval = false)
                : TBaseIterator<false>(InMap.Pairs.CreateIterator()), Map(InMap), bElementsHaveBeenRemoved(false), bRequiresRehashOnRemoval(bInRequiresRehashOnRemoval)
            {
            }

            inline ~TIterator()
            {
                if (bElementsHaveBeenRemoved && bRequiresRehashOnRemoval)
                {
                    Map.Pairs.Relax();
                }
            }

            /** Remove the current pair from the map */
            inline void RemoveCurrent()
            {
                TBaseIterator<false>::PairIt.RemoveCurrent();
                bElementsHaveBeenRemoved = true;
            }

          private:
            TMapBase& Map;
            bool bElementsHaveBeenRemoved;
            bool bRequiresRehashOnRemoval;
        };

        /** Const map iterator */
        class TConstIterator : public TBaseIterator<true>
        {
          public:
            [[nodiscard]] OLO_FINLINE TConstIterator(const TMapBase& InMap)
                : TBaseIterator<true>(InMap.Pairs.CreateConstIterator())
            {
            }
        };

        using TRangedForIterator = typename ElementSetType::TRangedForIterator;
        using TRangedForConstIterator = typename ElementSetType::TRangedForConstIterator;

        /** Iterates over values associated with a specified key in a const map. */
        class TConstKeyIterator : public TBaseKeyIterator<true>
        {
          private:
            using Super = TBaseKeyIterator<true>;
            using IteratorType = typename ElementSetType::TConstKeyIterator;

          public:
            using KeyArgumentType = typename IteratorType::KeyArgumentType;

            [[nodiscard]] OLO_FINLINE TConstKeyIterator(const TMapBase& InMap, KeyArgumentType InKey)
                : Super(IteratorType(InMap.Pairs, InKey))
            {
            }
        };

        /** Iterates over values associated with a specified key in a map. */
        class TKeyIterator : public TBaseKeyIterator<false>
        {
          private:
            using Super = TBaseKeyIterator<false>;
            using IteratorType = typename ElementSetType::TKeyIterator;

          public:
            using KeyArgumentType = typename IteratorType::KeyArgumentType;

            [[nodiscard]] OLO_FINLINE TKeyIterator(TMapBase& InMap, KeyArgumentType InKey)
                : Super(IteratorType(InMap.Pairs, InKey))
            {
            }

            /** Removes the current key-value pair from the map. */
            OLO_FINLINE void RemoveCurrent()
            {
                TBaseKeyIterator<false>::SetIt.RemoveCurrent();
            }
        };

        /** Create an iterator */
        [[nodiscard]] OLO_FINLINE TIterator CreateIterator()
        {
            return TIterator(*this);
        }

        /** Create a const iterator */
        [[nodiscard]] OLO_FINLINE TConstIterator CreateConstIterator() const
        {
            return TConstIterator(*this);
        }

        /** Creates an iterator over the values associated with a specified key in a map */
        [[nodiscard]] OLO_FINLINE TKeyIterator CreateKeyIterator(typename TKeyIterator::KeyArgumentType InKey)
        {
            return TKeyIterator(*this, InKey);
        }

        /** Creates a const iterator over the values associated with a specified key in a map */
        [[nodiscard]] OLO_FINLINE TConstKeyIterator CreateConstKeyIterator(typename TConstKeyIterator::KeyArgumentType InKey) const
        {
            return TConstKeyIterator(*this, InKey);
        }

        // ========================================================================
        // Range-based for loop support
        // ========================================================================

        [[nodiscard]] OLO_FINLINE TRangedForIterator begin()
        {
            return TRangedForIterator(Pairs.begin());
        }

        [[nodiscard]] OLO_FINLINE TRangedForConstIterator begin() const
        {
            return TRangedForConstIterator(Pairs.begin());
        }

        [[nodiscard]] OLO_FINLINE TRangedForIterator end()
        {
            return TRangedForIterator(Pairs.end());
        }

        [[nodiscard]] OLO_FINLINE TRangedForConstIterator end() const
        {
            return TRangedForConstIterator(Pairs.end());
        }

        // ========================================================================
        // Memory Image Support
        // ========================================================================

        void WriteMemoryImage(FMemoryImageWriter& Writer) const
        {
            Pairs.WriteMemoryImage(Writer);
        }

        void CopyUnfrozen(const FMemoryUnfreezeContent& Context, void* Dst) const
        {
            Pairs.CopyUnfrozen(Context, Dst);
        }

        static void AppendHash(const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
        {
            ElementSetType::AppendHash(LayoutParams, Hasher);
        }

        // Maps are deliberately prevented from being hashed or compared, because this would hide
        // potentially major performance problems behind default operations.
        friend u32 GetTypeHash(const TMapBase& Map) = delete;
        friend bool operator==(const TMapBase&, const TMapBase&) = delete;
        friend bool operator!=(const TMapBase&, const TMapBase&) = delete;
    };

    // ============================================================================
    // TSortableMapBase - Adds sorting capabilities
    // ============================================================================

    /**
     * @class TSortableMapBase
     * @brief Map base with sorting capabilities
     */
    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    class TSortableMapBase : public TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>
    {
      protected:
        using Super = TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>;

        [[nodiscard]] constexpr TSortableMapBase() = default;

        [[nodiscard]] explicit consteval TSortableMapBase(EConstEval)
            : Super(ConstEval)
        {
        }

        [[nodiscard]] TSortableMapBase(TSortableMapBase&&) = default;
        [[nodiscard]] TSortableMapBase(const TSortableMapBase&) = default;
        TSortableMapBase& operator=(TSortableMapBase&&) = default;
        TSortableMapBase& operator=(const TSortableMapBase&) = default;

        template<typename OtherSetAllocator>
        [[nodiscard]] TSortableMapBase(TSortableMapBase<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& Other)
            : Super(MoveTemp(Other))
        {
        }

        template<typename OtherSetAllocator>
        [[nodiscard]] TSortableMapBase(const TSortableMapBase<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& Other)
            : Super(Other)
        {
        }

        // ========================================================================
        // Intrusive TOptional<TSortableMapBase> state
        // ========================================================================
        static constexpr bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TSortableMapBase;

        [[nodiscard]] explicit TSortableMapBase(FIntrusiveUnsetOptionalState Tag)
            : Super(Tag)
        {
        }

        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState Tag) const
        {
            return Super::operator==(Tag);
        }

        template<typename OtherSetAllocator>
        TSortableMapBase& operator=(TSortableMapBase<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& Other)
        {
            Super::operator=(MoveTemp(Other));
            return *this;
        }

        template<typename OtherSetAllocator>
        TSortableMapBase& operator=(const TSortableMapBase<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& Other)
        {
            Super::operator=(Other);
            return *this;
        }

      public:
        /**
         * Sorts the pairs array using each pair's Key as the sort criteria, then rebuilds the map's hash.
         * Invoked using "MyMapVar.KeySort( PREDICATE_CLASS() );"
         */
        template<typename PREDICATE_CLASS>
        OLO_FINLINE void KeySort(const PREDICATE_CLASS& Predicate)
        {
            Super::Pairs.Sort(FKeyComparisonClass<PREDICATE_CLASS>(Predicate));
        }

        /**
         * Stable sorts the pairs array using each pair's Key as the sort criteria, then rebuilds the map's hash.
         * Invoked using "MyMapVar.KeyStableSort( PREDICATE_CLASS() );"
         */
        template<typename PREDICATE_CLASS>
        OLO_FINLINE void KeyStableSort(const PREDICATE_CLASS& Predicate)
        {
            Super::Pairs.StableSort(FKeyComparisonClass<PREDICATE_CLASS>(Predicate));
        }

        /**
         * Sorts the pairs array using each pair's Value as the sort criteria, then rebuilds the map's hash.
         * Invoked using "MyMapVar.ValueSort( PREDICATE_CLASS() );"
         */
        template<typename PREDICATE_CLASS>
        OLO_FINLINE void ValueSort(const PREDICATE_CLASS& Predicate)
        {
            Super::Pairs.Sort(FValueComparisonClass<PREDICATE_CLASS>(Predicate));
        }

        /**
         * Stable sorts the pairs array using each pair's Value as the sort criteria, then rebuilds the map's hash.
         * Invoked using "MyMapVar.ValueStableSort( PREDICATE_CLASS() );"
         */
        template<typename PREDICATE_CLASS>
        OLO_FINLINE void ValueStableSort(const PREDICATE_CLASS& Predicate)
        {
            Super::Pairs.StableSort(FValueComparisonClass<PREDICATE_CLASS>(Predicate));
        }

        /**
         * Sort the free element list so subsequent additions occur at lowest available indices.
         * @see TSparseArray::SortFreeList() for more info.
         */
        void SortFreeList()
        {
            Super::Pairs.SortFreeList();
        }

      private:
        /** Extracts the pair's key from the map's pair structure and passes it to the user provided comparison class. */
        template<typename PREDICATE_CLASS>
        class FKeyComparisonClass
        {
            TDereferenceWrapper<KeyType, PREDICATE_CLASS> Predicate;

          public:
            [[nodiscard]] OLO_FINLINE FKeyComparisonClass(const PREDICATE_CLASS& InPredicate)
                : Predicate(InPredicate)
            {
            }

            [[nodiscard]] inline bool operator()(const typename Super::ElementType& A, const typename Super::ElementType& B) const
            {
                return Predicate(A.Key, B.Key);
            }
        };

        /** Extracts the pair's value from the map's pair structure and passes it to the user provided comparison class. */
        template<typename PREDICATE_CLASS>
        class FValueComparisonClass
        {
            TDereferenceWrapper<ValueType, PREDICATE_CLASS> Predicate;

          public:
            [[nodiscard]] OLO_FINLINE FValueComparisonClass(const PREDICATE_CLASS& InPredicate)
                : Predicate(InPredicate)
            {
            }

            [[nodiscard]] inline bool operator()(const typename Super::ElementType& A, const typename Super::ElementType& B) const
            {
                return Predicate(A.Value, B.Value);
            }
        };
    };

    // ============================================================================
    // TMap - Main map class (single value per key)
    // ============================================================================

    /**
     * @class TMap
     * @brief A map from keys to values with only one value per key
     *
     * Uses a TSet of key-value pairs with custom KeyFuncs for O(1) operations.
     *
     * @tparam InKeyType    The key type
     * @tparam InValueType  The value type
     * @tparam SetAllocator Allocator policy for the underlying set
     * @tparam KeyFuncs     Functions for getting keys and computing hashes
     */
    template<
        typename InKeyType,
        typename InValueType,
        typename SetAllocator = FDefaultSetAllocator,
        typename KeyFuncs = TDefaultMapKeyFuncs<InKeyType, InValueType, false>>
    class TMap : public TSortableMapBase<InKeyType, InValueType, SetAllocator, KeyFuncs>
    {
        static_assert(!KeyFuncs::bAllowDuplicateKeys, "TMap cannot be instantiated with KeyFuncs that allows duplicate keys");

      public:
        using KeyType = InKeyType;
        using ValueType = InValueType;
        using SetAllocatorType = SetAllocator;
        using KeyFuncsType = KeyFuncs;

        using Super = TSortableMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>;
        using KeyInitType = typename Super::KeyInitType;
        using KeyConstPointerType = typename Super::KeyConstPointerType;
        using ElementType = typename Super::ElementType;

        [[nodiscard]] constexpr TMap() = default;

        [[nodiscard]] explicit consteval TMap(EConstEval)
            : Super(ConstEval)
        {
        }

        [[nodiscard]] TMap(TMap&&) = default;
        [[nodiscard]] TMap(const TMap&) = default;
        TMap& operator=(TMap&&) = default;
        TMap& operator=(const TMap&) = default;

        // ========================================================================
        // Intrusive TOptional<TMap> state
        // ========================================================================
        static constexpr bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TMap;

        [[nodiscard]] explicit TMap(FIntrusiveUnsetOptionalState Tag)
            : Super(Tag)
        {
        }

        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState Tag) const
        {
            return Super::operator==(Tag);
        }

        /** Constructor for moving elements from a TMap with a different SetAllocator */
        template<typename OtherSetAllocator>
        [[nodiscard]] TMap(TMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& Other)
            : Super(MoveTemp(Other))
        {
        }

        /** Constructor for copying elements from a TMap with a different SetAllocator */
        template<typename OtherSetAllocator>
        [[nodiscard]] TMap(const TMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& Other)
            : Super(Other)
        {
        }

        /** Initializer list constructor */
        [[nodiscard]] TMap(std::initializer_list<TPairInitializer<const KeyType&, const ValueType&>> InitList)
        {
            this->Reserve(static_cast<i32>(InitList.size()));
            for (const auto& Element : InitList)
            {
                this->Add(Element.Key, Element.Value);
            }
        }

        /** Assignment operator for moving elements from a TMap with a different SetAllocator */
        template<typename OtherSetAllocator>
        TMap& operator=(TMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& Other)
        {
            Super::operator=(MoveTemp(Other));
            return *this;
        }

        /** Assignment operator for copying elements from a TMap with a different SetAllocator */
        template<typename OtherSetAllocator>
        TMap& operator=(const TMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& Other)
        {
            Super::operator=(Other);
            return *this;
        }

        /** Initializer list assignment */
        TMap& operator=(std::initializer_list<TPairInitializer<const KeyType&, const ValueType&>> InitList)
        {
            this->Empty(static_cast<i32>(InitList.size()));
            for (const auto& Element : InitList)
            {
                this->Add(Element.Key, Element.Value);
            }
            return *this;
        }

        /**
         * @brief Remove a key and copy its value to output parameter
         * @param Key             The key to remove
         * @param OutRemovedValue Will contain the removed value if found
         * @return true if key was found and removed
         */
        inline bool RemoveAndCopyValue(KeyInitType Key, ValueType& OutRemovedValue)
        {
            const FSetElementId PairId = Super::Pairs.FindId(Key);
            if (!PairId.IsValidId())
            {
                return false;
            }

            OutRemovedValue = MoveTemp(Super::Pairs[PairId].Value);
            Super::Pairs.Remove(PairId);
            return true;
        }

        /** RemoveAndCopyValue, set remains compact and in stable order after element is removed */
        inline bool RemoveAndCopyValueStable(KeyInitType Key, ValueType& OutRemovedValue)
        {
            const FSetElementId PairId = Super::Pairs.FindId(Key);
            if (!PairId.IsValidId())
            {
                return false;
            }

            OutRemovedValue = MoveTemp(Super::Pairs[PairId].Value);
            Super::Pairs.RemoveStable(PairId);
            return true;
        }

        /** RemoveAndCopyValue with precomputed hash */
        template<typename ComparableKey>
        inline bool RemoveAndCopyValueByHash(u32 KeyHash, const ComparableKey& Key, ValueType& OutRemovedValue)
        {
            const FSetElementId PairId = Super::Pairs.FindIdByHash(KeyHash, Key);
            if (!PairId.IsValidId())
            {
                return false;
            }

            OutRemovedValue = MoveTemp(Super::Pairs[PairId].Value);
            Super::Pairs.Remove(PairId);
            return true;
        }

        /**
         * @brief Find and remove a key, returning its value
         * @param Key  The key to search for
         * @return The value that was removed (asserts if not found)
         */
        inline ValueType FindAndRemoveChecked(KeyConstPointerType Key)
        {
            const FSetElementId PairId = Super::Pairs.FindId(Key);
            OLO_CORE_ASSERT(PairId.IsValidId(), "Key not found in map");
            ValueType Result = MoveTemp(Super::Pairs[PairId].Value);
            Super::Pairs.Remove(PairId);
            return Result;
        }

        /**
         * @brief Move all items from another map into this one
         * @param OtherMap  The other map (will be emptied)
         */
        template<typename OtherSetAllocator>
        void Append(TMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& OtherMap)
        {
            this->Reserve(this->Num() + OtherMap.Num());
            for (auto& Pair : OtherMap)
            {
                this->Add(MoveTemp(Pair.Key), MoveTemp(Pair.Value));
            }
            OtherMap.Reset();
        }

        /**
         * @brief Add all items from another map
         * @param OtherMap  The other map
         */
        template<typename OtherSetAllocator>
        void Append(const TMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& OtherMap)
        {
            this->Reserve(this->Num() + OtherMap.Num());
            for (const auto& Pair : OtherMap)
            {
                this->Add(Pair.Key, Pair.Value);
            }
        }

        /** Subscript operator - asserts if key not found */
        [[nodiscard]] OLO_FINLINE ValueType& operator[](KeyConstPointerType Key)
        {
            return this->FindChecked(Key);
        }

        [[nodiscard]] OLO_FINLINE const ValueType& operator[](KeyConstPointerType Key) const
        {
            return this->FindChecked(Key);
        }

        /**
         * Filters the elements in the map based on a predicate functor.
         *
         * @param Pred  The functor to apply to each element.
         * @returns TMap with the same type as this object which contains
         *          the subset of elements for which the functor returns true.
         */
        template<typename Predicate>
        [[nodiscard]] TMap<KeyType, ValueType, SetAllocator, KeyFuncs> FilterByPredicate(Predicate Pred) const
        {
            TMap<KeyType, ValueType, SetAllocator, KeyFuncs> FilterResults;
            FilterResults.Reserve(Super::Pairs.Num());
            for (const ElementType& Pair : Super::Pairs)
            {
                if (Pred(Pair))
                {
                    FilterResults.Add(Pair.Key, Pair.Value);
                }
            }
            return FilterResults;
        }

        /** Comparison */
        [[nodiscard]] bool operator==(const TMap& Other) const
        {
            if (this->Num() != Other.Num())
            {
                return false;
            }

            for (const auto& Pair : *this)
            {
                const ValueType* OtherValue = Other.Find(Pair.Key);
                if (OtherValue == nullptr || !(*OtherValue == Pair.Value))
                {
                    return false;
                }
            }

            return true;
        }

        [[nodiscard]] bool operator!=(const TMap& Other) const
        {
            return !(*this == Other);
        }
    };

    // ============================================================================
    // TMultiMap - Map allowing multiple values per key
    // ============================================================================

    /**
     * @class TMultiMap
     * @brief A map from keys to values allowing multiple values per key
     */
    template<
        typename KeyType,
        typename ValueType,
        typename SetAllocator = FDefaultSetAllocator,
        typename KeyFuncs = TDefaultMapKeyFuncs<KeyType, ValueType, true>>
    class TMultiMap : public TSortableMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>
    {
        static_assert(KeyFuncs::bAllowDuplicateKeys, "TMultiMap cannot be instantiated with KeyFuncs that disallows duplicate keys");

      public:
        using Super = TSortableMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>;
        using Super::Num; // Bring back the no-arg Num() from base class
        using KeyConstPointerType = typename Super::KeyConstPointerType;
        using KeyInitType = typename Super::KeyInitType;
        using ValueInitType = typename Super::ValueInitType;
        using ElementType = typename Super::ElementType;

        [[nodiscard]] constexpr TMultiMap() = default;
        [[nodiscard]] TMultiMap(TMultiMap&&) = default;
        [[nodiscard]] TMultiMap(const TMultiMap&) = default;
        TMultiMap& operator=(TMultiMap&&) = default;
        TMultiMap& operator=(const TMultiMap&) = default;

        // ========================================================================
        // Intrusive TOptional<TMultiMap> state
        // ========================================================================
        static constexpr bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TMultiMap;

        [[nodiscard]] explicit TMultiMap(FIntrusiveUnsetOptionalState Tag)
            : Super(Tag)
        {
        }

        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState Tag) const
        {
            return Super::operator==(Tag);
        }

        template<typename OtherSetAllocator>
        [[nodiscard]] TMultiMap(TMultiMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& Other)
            : Super(MoveTemp(Other))
        {
        }

        template<typename OtherSetAllocator>
        [[nodiscard]] TMultiMap(const TMultiMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& Other)
            : Super(Other)
        {
        }

        [[nodiscard]] TMultiMap(std::initializer_list<TPairInitializer<const KeyType&, const ValueType&>> InitList)
        {
            this->Reserve(static_cast<i32>(InitList.size()));
            for (const auto& Element : InitList)
            {
                this->Add(Element.Key, Element.Value);
            }
        }

        template<typename OtherSetAllocator>
        TMultiMap& operator=(TMultiMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>&& Other)
        {
            Super::operator=(MoveTemp(Other));
            return *this;
        }

        template<typename OtherSetAllocator>
        TMultiMap& operator=(const TMultiMap<KeyType, ValueType, OtherSetAllocator, KeyFuncs>& Other)
        {
            Super::operator=(Other);
            return *this;
        }

        /**
         * @brief Find all values associated with a key
         * @param Key        The key to search for
         * @param OutValues  Will contain all values for the key
         * @param bMaintainOrder  If true, maintain insertion order
         */
        template<typename Allocator>
        void MultiFind(KeyInitType Key, TArray<ValueType, Allocator>& OutValues, bool bMaintainOrder = false) const
        {
            for (typename Super::ElementSetType::TConstKeyIterator It(Super::Pairs, Key); It; ++It)
            {
                OutValues.Add(It->Value);
            }

            if (bMaintainOrder)
            {
                Algo::Reverse(OutValues);
            }
        }

        /**
         * @brief Find all values associated with a key, returning pointers
         * @param Key        The key to search for
         * @param OutValues  Will contain pointers to all values for the key
         * @param bMaintainOrder  If true, maintain insertion order
         */
        template<typename Allocator>
        void MultiFindPointer(KeyInitType Key, TArray<const ValueType*, Allocator>& OutValues, bool bMaintainOrder = false) const
        {
            for (typename Super::ElementSetType::TConstKeyIterator It(Super::Pairs, Key); It; ++It)
            {
                OutValues.Add(&It->Value);
            }

            if (bMaintainOrder)
            {
                Algo::Reverse(OutValues);
            }
        }

        template<typename Allocator>
        void MultiFindPointer(KeyInitType Key, TArray<ValueType*, Allocator>& OutValues, bool bMaintainOrder = false)
        {
            for (typename Super::ElementSetType::TKeyIterator It(Super::Pairs, Key); It; ++It)
            {
                OutValues.Add(&It->Value);
            }

            if (bMaintainOrder)
            {
                Algo::Reverse(OutValues);
            }
        }

        /**
         * @brief Add a key-value pair only if it doesn't already exist
         *
         * If both the key and value match an existing association in the map,
         * no new association is made and the existing association's value is returned.
         *
         * @param InKey    The key
         * @param InValue  The value
         * @return Reference to the value (existing or newly added)
         */
        OLO_FINLINE ValueType& AddUnique(const KeyType& InKey, const ValueType& InValue)
        {
            return EmplaceUnique(InKey, InValue);
        }

        OLO_FINLINE ValueType& AddUnique(const KeyType& InKey, ValueType&& InValue)
        {
            return EmplaceUnique(InKey, MoveTemp(InValue));
        }

        OLO_FINLINE ValueType& AddUnique(KeyType&& InKey, const ValueType& InValue)
        {
            return EmplaceUnique(MoveTemp(InKey), InValue);
        }

        OLO_FINLINE ValueType& AddUnique(KeyType&& InKey, ValueType&& InValue)
        {
            return EmplaceUnique(MoveTemp(InKey), MoveTemp(InValue));
        }

        /**
         * @brief Emplace a key-value pair only if it doesn't already exist
         *
         * If both the key and value match an existing association in the map,
         * no new association is made and the existing association's value is returned.
         *
         * @param InKey    The key to associate
         * @param InValue  The value to associate
         * @return Reference to the value as stored in the map
         */
        template<typename InitKeyType, typename InitValueType>
        ValueType& EmplaceUnique(InitKeyType&& InKey, InitValueType&& InValue)
        {
            if (ValueType* Found = FindPair(InKey, InValue))
            {
                return *Found;
            }
            return Super::Add(Forward<InitKeyType>(InKey), Forward<InitValueType>(InValue));
        }

        /**
         * @brief Find an association between a specified key and value (const)
         * @param Key    The key to find
         * @param Value  The value to find
         * @return Pointer to the value if found, nullptr otherwise
         */
        [[nodiscard]] OLO_FINLINE const ValueType* FindPair(KeyInitType Key, ValueInitType Value) const
        {
            return const_cast<TMultiMap*>(this)->FindPair(Key, Value);
        }

        /**
         * @brief Find an association between a specified key and value
         * @param Key    The key to find
         * @param Value  The value to find
         * @return Pointer to the value if found, nullptr otherwise
         */
        [[nodiscard]] ValueType* FindPair(KeyInitType Key, ValueInitType Value)
        {
            // Iterate over pairs with a matching key
            for (typename Super::ElementSetType::TKeyIterator It(Super::Pairs, Key); It; ++It)
            {
                // If the pair's value matches, return a pointer to it
                if (It->Value == Value)
                {
                    return &It->Value;
                }
            }
            return nullptr;
        }

        /**
         * @brief Remove all associations between a key and value
         * @param InKey    The key
         * @param InValue  The value to match
         * @return Number of pairs removed
         */
        i32 Remove(KeyInitType InKey, ValueInitType InValue)
        {
            i32 NumRemovedPairs = 0;
            for (typename Super::ElementSetType::TKeyIterator It(Super::Pairs, InKey); It; ++It)
            {
                if (It->Value == InValue)
                {
                    It.RemoveCurrent();
                    ++NumRemovedPairs;
                }
            }
            return NumRemovedPairs;
        }

        /**
         * @brief Remove a specific key-value pair
         * @param InKey    The key
         * @param InValue  The value to match
         * @return Number of pairs removed
         */
        i32 RemoveSingle(KeyInitType InKey, ValueInitType InValue)
        {
            for (typename Super::ElementSetType::TKeyIterator It(Super::Pairs, InKey); It; ++It)
            {
                if (It->Value == InValue)
                {
                    It.RemoveCurrent();
                    return 1;
                }
            }
            return 0;
        }

        /**
         * @brief Remove first association between key and value, preserving order
         * @param InKey    The key
         * @param InValue  The value to match
         * @return true if a pair was removed
         */
        bool RemoveSingleStable(KeyInitType InKey, ValueInitType InValue)
        {
            for (typename Super::ElementSetType::TKeyIterator It(Super::Pairs, InKey); It; ++It)
            {
                if (It->Value == InValue)
                {
                    Super::Pairs.RemoveStable(It.GetId());
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Count how many values are associated with a key
         * @param Key  The key to count values for
         * @return Number of values for the key
         */
        [[nodiscard]] i32 Num(KeyInitType Key) const
        {
            i32 NumMatchingPairs = 0;
            for (typename Super::ElementSetType::TConstKeyIterator It(Super::Pairs, Key); It; ++It)
            {
                ++NumMatchingPairs;
            }
            return NumMatchingPairs;
        }
    };

    // ============================================================================
    // TIsTMap Specializations
    // ============================================================================

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    struct TIsTMap<TMap<KeyType, ValueType, SetAllocator, KeyFuncs>>
    {
        static constexpr bool Value = true;
    };

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    struct TIsTMap<const TMap<KeyType, ValueType, SetAllocator, KeyFuncs>>
    {
        static constexpr bool Value = true;
    };

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    struct TIsTMap<volatile TMap<KeyType, ValueType, SetAllocator, KeyFuncs>>
    {
        static constexpr bool Value = true;
    };

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    struct TIsTMap<const volatile TMap<KeyType, ValueType, SetAllocator, KeyFuncs>>
    {
        static constexpr bool Value = true;
    };

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    struct TIsTMap<TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>>
    {
        static constexpr bool Value = true;
    };

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    struct TIsTMap<const TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>>
    {
        static constexpr bool Value = true;
    };

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    struct TIsTMap<volatile TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>>
    {
        static constexpr bool Value = true;
    };

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    struct TIsTMap<const volatile TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>>
    {
        static constexpr bool Value = true;
    };

    // ============================================================================
    // TMapPrivateFriend - Friend struct for serialization access
    // ============================================================================

    struct TMapPrivateFriend
    {
        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        inline static FArchive& Serialize(FArchive& Ar, TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& Map)
        {
            Ar << Map.Pairs;
            return Ar;
        }

        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        OLO_FINLINE static void SerializeStructured(FStructuredArchive::FSlot Slot, TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& InMap)
        {
            Slot << InMap.Pairs;
        }

        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        [[nodiscard]] static bool LegacyCompareEqual(const TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& A, const TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& B)
        {
            return TSetPrivateFriend::LegacyCompareEqual(A.Pairs, B.Pairs);
        }
    };

    // ============================================================================
    // Serialization Operators
    // ============================================================================

    /** Serializer. */
    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    OLO_FINLINE FArchive& operator<<(FArchive& Ar, TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& Map)
    {
        return TMapPrivateFriend::Serialize(Ar, Map);
    }

    /** Structured archive serializer. */
    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    OLO_FINLINE void operator<<(FStructuredArchive::FSlot Slot, TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& InMap)
    {
        TMapPrivateFriend::SerializeStructured(Slot, InMap);
    }

    // ============================================================================
    // Legacy Comparison Functions
    // ============================================================================

    /** Legacy comparison operators. Note that these also test whether the map's key-value pairs were added in the same order! */
    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    [[nodiscard]] bool LegacyCompareEqual(const TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& A, const TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& B)
    {
        return TMapPrivateFriend::LegacyCompareEqual(A, B);
    }

    template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
    [[nodiscard]] bool LegacyCompareNotEqual(const TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& A, const TMapBase<KeyType, ValueType, SetAllocator, KeyFuncs>& B)
    {
        return !TMapPrivateFriend::LegacyCompareEqual(A, B);
    }

    // ============================================================================
    // Freeze Namespace - Memory Image Functions
    // ============================================================================

    namespace Freeze
    {
        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        void IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer, const TMap<KeyType, ValueType, SetAllocator, KeyFuncs>& Object, const FTypeLayoutDesc&)
        {
            Object.WriteMemoryImage(Writer);
        }

        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        u32 IntrinsicUnfrozenCopy(const FMemoryUnfreezeContent& Context, const TMap<KeyType, ValueType, SetAllocator, KeyFuncs>& Object, void* OutDst)
        {
            Object.CopyUnfrozen(Context, OutDst);
            return sizeof(Object);
        }

        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        u32 IntrinsicAppendHash(const TMap<KeyType, ValueType, SetAllocator, KeyFuncs>* DummyObject, const FTypeLayoutDesc& TypeDesc, const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
        {
            TMap<KeyType, ValueType, SetAllocator, KeyFuncs>::AppendHash(LayoutParams, Hasher);
            return DefaultAppendHash(TypeDesc, LayoutParams, Hasher);
        }

        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        void IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer, const TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>& Object, const FTypeLayoutDesc&)
        {
            Object.WriteMemoryImage(Writer);
        }

        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        u32 IntrinsicUnfrozenCopy(const FMemoryUnfreezeContent& Context, const TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>& Object, void* OutDst)
        {
            Object.CopyUnfrozen(Context, OutDst);
            return sizeof(Object);
        }

        template<typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
        u32 IntrinsicAppendHash(const TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>* DummyObject, const FTypeLayoutDesc& TypeDesc, const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
        {
            TMultiMap<KeyType, ValueType, SetAllocator, KeyFuncs>::AppendHash(LayoutParams, Hasher);
            return DefaultAppendHash(TypeDesc, LayoutParams, Hasher);
        }
    } // namespace Freeze

    // Note: GetTypeHash for TPair is provided by Tuple.h via TTuple's hash function

} // namespace OloEngine
