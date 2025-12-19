#pragma once

/**
 * @file SetElement.h
 * @brief Set element wrapper and key functions for TSet containers
 *
 * This file now acts as a redirect to the properly separated UE-style headers:
 * - SetUtilities.h: FSetElementId, BaseKeyFuncs, DefaultKeyFuncs, MoveByRelocate
 * - SparseSetElement.h: TSparseSetElement for sparse set storage
 *
 * For backward compatibility, this header also provides:
 * - TSetElement as an alias for TSparseSetElement
 * - SetHelpers as an alias for SparseSetPrivate (deprecated)
 *
 * Ported from Unreal Engine's Containers/SetUtilities.h and SparseSetElement.h
 */

#include "OloEngine/Containers/SetUtilities.h"
#include "OloEngine/Containers/SparseSetElement.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"

namespace OloEngine
{
    // ============================================================================
    // Backward Compatibility Aliases
    // ============================================================================

    /**
     * @brief TSetElement is an alias for TSparseSetElement for backward compatibility
     *
     * New code should use TSparseSetElement directly.
     */
    template<typename InElementType>
    using TSetElement = TSparseSetElement<InElementType>;

    /**
     * @brief SetHelpers namespace provides aliases to SparseSetPrivate for backward compatibility
     *
     * New code should use the SparseSetPrivate namespace directly.
     */
    namespace SetHelpers
    {
        using SparseSetPrivate::CopyHash;
        using SparseSetPrivate::GetTypedHash;
        using SparseSetPrivate::OnInvalidSetNum;
        using SparseSetPrivate::Rehash;
    } // namespace SetHelpers

    // TIsSparseSet and TIsCompactSet are now defined in SetUtilities.h
    // Specializations are in their respective container headers

} // namespace OloEngine
