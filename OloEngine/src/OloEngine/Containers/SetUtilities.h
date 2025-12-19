#pragma once

/**
 * @file SetUtilities.h
 * @brief Utility types for TSet containers
 *
 * Provides:
 * - TIsTSet: Traits class to detect TSet types
 * - BaseKeyFuncs: Base class for custom key functions
 * - DefaultKeyFuncs: Default key functions using element as key
 * - MoveByRelocate: Move-by-relocate helper
 * - FSetElementId: Opaque identifier for set elements
 *
 * Note: EAllowShrinking is defined in Array.h and is also used by set containers.
 *
 * Ported from Unreal Engine's Containers/SetUtilities.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Serialization/MemoryLayout.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Templates/TypeHash.h"
// Note: Tuple.h support not yet implemented - will be added when TTuple is ported

namespace OloEngine
{
    // ============================================================================
    // Type Traits
    // ============================================================================

    /**
     * @struct TIsTSet
     * @brief Traits class which determines whether or not a type is a TSet
     */
    template<typename T>
    struct TIsTSet
    {
        enum
        {
            Value = false
        };
    };

    /**
     * @struct TIsSparseSet
     * @brief Traits class which determines whether or not a type is a TSparseSet
     */
    template<typename T>
    struct TIsSparseSet
    {
        enum
        {
            Value = false
        };
    };

    /**
     * @struct TIsCompactSet
     * @brief Traits class which determines whether or not a type is a TCompactSet
     */
    template<typename T>
    struct TIsCompactSet
    {
        enum
        {
            Value = false
        };
    };

    /**
     * @struct BaseKeyFuncs
     * @brief The base KeyFuncs type with useful definitions; meant to be derived from
     *
     * bInAllowDuplicateKeys=true is slightly faster because it allows the TSet to skip
     * validating that there isn't already a duplicate entry in the TSet.
     *
     * @tparam ElementType          The element type stored in the set
     * @tparam InKeyType            The type used as a key
     * @tparam bInAllowDuplicateKeys Whether to allow duplicate keys (faster if true)
     */
    template<typename ElementType, typename InKeyType, bool bInAllowDuplicateKeys = false>
    struct BaseKeyFuncs
    {
        using KeyType = InKeyType;
        using KeyInitType = typename TCallTraits<InKeyType>::ParamType;
        using ElementInitType = typename TCallTraits<ElementType>::ParamType;

        enum
        {
            bAllowDuplicateKeys = bInAllowDuplicateKeys
        };
    };

    /**
     * @struct DefaultKeyFuncs
     * @brief A default implementation of the KeyFuncs used by TSet which uses the element as a key
     *
     * @tparam ElementType          The element type stored in the set
     * @tparam bInAllowDuplicateKeys Whether to allow duplicate keys
     */
    template<typename ElementType, bool bInAllowDuplicateKeys = false>
    struct DefaultKeyFuncs : BaseKeyFuncs<ElementType, ElementType, bInAllowDuplicateKeys>
    {
        using KeyInitType = typename TTypeTraits<ElementType>::ConstPointerType;
        using ElementInitType = typename TCallTraits<ElementType>::ParamType;

        /**
         * @return The key used to index the given element
         */
        [[nodiscard]] static OLO_FINLINE KeyInitType GetSetKey(ElementInitType Element)
        {
            return Element;
        }

        /**
         * @return True if the keys match
         */
        [[nodiscard]] static OLO_FINLINE bool Matches(KeyInitType A, KeyInitType B)
        {
            return A == B;
        }

        /**
         * @return True if the keys match (heterogeneous comparison)
         */
        template<typename ComparableKey>
        [[nodiscard]] static OLO_FINLINE bool Matches(KeyInitType A, ComparableKey B)
        {
            return A == B;
        }

        /** Calculates a hash index for a key */
        [[nodiscard]] static OLO_FINLINE u32 GetKeyHash(KeyInitType Key)
        {
            return GetTypeHash(Key);
        }

        /** Calculates a hash index for a key (heterogeneous) */
        template<typename ComparableKey>
        [[nodiscard]] static OLO_FINLINE u32 GetKeyHash(ComparableKey Key)
        {
            return GetTypeHash(Key);
        }
    };

    /**
     * @brief Move element A to B by relocating (destroying A)
     *
     * This is used to provide type specific behavior for a move which will destroy B.
     */
    template<typename T>
    inline void MoveByRelocate(T& A, T& B)
    {
        // Destruct the previous value of A
        A.~T();

        // Relocate B into the 'hole' left by the destruction of A, leaving a hole in B instead
        RelocateConstructItems<T>(&A, &B, 1);
    }

    /**
     * @class FSetElementId
     * @brief Either NULL or an identifier for an element of a set
     *
     * Used to differentiate between int as an element type and an index to a specific location.
     */
    class FSetElementId
    {
      public:
        /** Default constructor - creates an invalid id */
        constexpr OLO_FINLINE FSetElementId() = default;

        /** @return true if this is a valid element id */
        [[nodiscard]] OLO_FINLINE bool IsValidId() const
        {
            return Index != INDEX_NONE;
        }

        /** Comparison operators */
        [[nodiscard]] OLO_FINLINE friend bool operator==(const FSetElementId& A, const FSetElementId& B)
        {
            return A.Index == B.Index;
        }

        [[nodiscard]] OLO_FINLINE friend bool operator!=(const FSetElementId& A, const FSetElementId& B)
        {
            return A.Index != B.Index;
        }

        /** @return the underlying index value */
        [[nodiscard]] constexpr OLO_FINLINE i32 AsInteger() const
        {
            return Index;
        }

        /** Create an FSetElementId from an integer */
        [[nodiscard]] OLO_FINLINE static FSetElementId FromInteger(i32 Integer)
        {
            return FSetElementId(Integer);
        }

      private:
        /** The index of the element in the set's element array */
        i32 Index = INDEX_NONE;

        /** Private constructor from integer */
        OLO_FINLINE explicit FSetElementId(i32 InIndex)
            : Index(InIndex)
        {
        }
    };

} // namespace OloEngine

// FSetElementId is just an int32 - declare intrinsic type layout
DECLARE_INTRINSIC_TYPE_LAYOUT(OloEngine::FSetElementId);
