#pragma once

/**
 * @file Set.h
 * @brief Hash-based set container
 *
 * TSet is TSparseSet: elements live in a TSparseArray, so element IDs stay valid when other
 * elements are removed, and iteration order is stable under a stable remove.
 *
 * Ported from Unreal Engine's Containers/Set.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Containers/SetUtilities.h"
#include "OloEngine/Containers/SparseSet.h"

namespace OloEngine
{
    template<
        typename ElementType,
        typename KeyFuncs = DefaultKeyFuncs<ElementType>,
        typename Allocator = FDefaultSetAllocator>
    using TSet = TSparseSet<ElementType, KeyFuncs, Allocator>;

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
