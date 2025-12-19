#pragma once

/**
 * @file Set.h
 * @brief Hash-based set container with selectable implementation
 *
 * This header provides the TSet alias which can be configured to use either:
 * - TCompactSet: Better memory efficiency, elements stored contiguously (default)
 * - TSparseSet: Preserves element order on removal, uses sparse array
 *
 * Set OLO_USE_COMPACT_SET_AS_DEFAULT to 0 to use TSparseSet as the default.
 *
 * Ported from Unreal Engine's Containers/Set.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Containers/SetUtilities.h"

// ============================================================================
// Implementation Selection
// ============================================================================

/**
 * @def OLO_USE_COMPACT_SET_AS_DEFAULT
 * @brief Controls which set implementation TSet aliases to
 *
 * When 1 (default): TSet = TCompactSet (better memory, contiguous storage)
 * When 0:           TSet = TSparseSet (preserves order, sparse array storage)
 *
 * TCompactSet advantages:
 * - Better cache locality (contiguous elements)
 * - Lower memory overhead (adaptive index type)
 * - Faster iteration
 *
 * TSparseSet advantages:
 * - Preserves element order on removal (when using stable remove)
 * - Element IDs remain valid after other elements are removed
 * - Better for cases where ID stability is important
 */
#ifndef OLO_USE_COMPACT_SET_AS_DEFAULT
#define OLO_USE_COMPACT_SET_AS_DEFAULT 1
#endif

// ============================================================================
// Include the appropriate implementation
// ============================================================================

#if OLO_USE_COMPACT_SET_AS_DEFAULT

// Use TCompactSet as the default TSet implementation
#include "OloEngine/Containers/CompactSet.h"

namespace OloEngine
{
    /**
     * @brief TSet is an alias for TCompactSet when OLO_USE_COMPACT_SET_AS_DEFAULT is enabled
     *
     * Note: TCompactSet only supports FDefaultAllocator, so the Allocator parameter is ignored
     */
    template<
        typename ElementType,
        typename KeyFuncs = DefaultKeyFuncs<ElementType>,
        typename Allocator = FDefaultSetAllocator>
    using TSet = TCompactSet<ElementType, KeyFuncs, FDefaultAllocator>;

    // TIsTSet specializations for TCompactSet are already in CompactSet.h

} // namespace OloEngine

// Also include TSparseSet for explicit use
#include "OloEngine/Containers/SparseSet.h"

#else // !OLO_USE_COMPACT_SET_AS_DEFAULT

// Use TSparseSet as the default TSet implementation
#include "OloEngine/Containers/SparseSet.h"

namespace OloEngine
{
    /**
     * @brief TSet is an alias for TSparseSet when OLO_USE_COMPACT_SET_AS_DEFAULT is disabled
     */
    template<
        typename ElementType,
        typename KeyFuncs = DefaultKeyFuncs<ElementType>,
        typename Allocator = FDefaultSetAllocator>
    using TSet = TSparseSet<ElementType, KeyFuncs, Allocator>;

    // TIsTSet specializations for TSparseSet are already in SparseSet.h

} // namespace OloEngine

// Also include TCompactSet for explicit use
#include "OloEngine/Containers/CompactSet.h"

#endif // OLO_USE_COMPACT_SET_AS_DEFAULT

namespace OloEngine
{
    // ============================================================================
    // Common GetTypeHash for sets (works with either implementation)
    // ============================================================================

    /**
     * @brief Hash function for sets
     *
     * Computes a hash by XORing all element hashes together.
     * Note: Order-independent hash since set order may not be stable.
     */
    template<typename SetType>
    [[nodiscard]] inline std::enable_if_t<TIsTSet<SetType>::Value, u32>
    GetTypeHash(const SetType& Set)
    {
        u32 Hash = 0;
        for (const auto& Element : Set)
        {
            Hash ^= GetTypeHash(Element);
        }
        return Hash;
    }

} // namespace OloEngine
