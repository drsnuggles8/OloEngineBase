#pragma once

/**
 * @file ContainerElementTypeCompatibility.h
 * @brief Container element type compatibility traits
 *
 * Temporary compatibility mechanism to be used solely for the purpose of raw pointers to wrapped pointers.
 * Specialization and use of this type is not supported beyond specific wrapper types.
 *
 * Ported from Unreal Engine's Containers/ContainerElementTypeCompatibility.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"

namespace OloEngine
{

    /**
     * @struct TContainerElementTypeCompatibility
     * @brief Default trait for container element type compatibility
     *
     * For most types, ReinterpretType and CopyFromOtherType are the same as the element type.
     * Specialize this for wrapper types (like object pointers) that need special handling.
     */
    template<typename InElementType>
    struct TContainerElementTypeCompatibility
    {
        typedef InElementType ReinterpretType;
        typedef InElementType CopyFromOtherType;

        template<typename IterBeginType, typename IterEndType, typename OperatorType = InElementType& (*)(IterBeginType&)>
        static void ReinterpretRange(IterBeginType Iter, IterEndType IterEnd, OperatorType Operator = [](IterBeginType& InIt) -> InElementType&
                                     { return *InIt; })
        {
            (void)Iter;
            (void)IterEnd;
            (void)Operator;
        }

        template<typename IterBeginType, typename IterEndType, typename SizeType, typename OperatorType = InElementType& (*)(IterBeginType&)>
        static void ReinterpretRangeContiguous(IterBeginType Iter, IterEndType IterEnd, SizeType Size, OperatorType Operator = [](IterBeginType& InIt) -> InElementType&
                                               { return *InIt; })
        {
            (void)Iter;
            (void)IterEnd;
            (void)Size;
            (void)Operator;
        }

        static constexpr void CopyingFromOtherType() {}
    };

    /**
     * @brief Check if element type is reinterpretable to another type
     *
     * This is true when the ReinterpretType differs from the ElementType.
     */
    template<typename ElementType>
    constexpr bool TIsContainerElementTypeReinterpretable_V = !std::is_same_v<typename TContainerElementTypeCompatibility<ElementType>::ReinterpretType, ElementType>;

    /**
     * @brief Check if element type can be copied from another type
     *
     * This is true when the CopyFromOtherType differs from the ElementType.
     */
    template<typename ElementType>
    constexpr bool TIsContainerElementTypeCopyable_V = !std::is_same_v<typename TContainerElementTypeCompatibility<ElementType>::CopyFromOtherType, ElementType>;

} // namespace OloEngine
