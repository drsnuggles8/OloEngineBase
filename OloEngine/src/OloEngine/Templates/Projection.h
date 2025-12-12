#pragma once

/**
 * @file Projection.h
 * @brief Projection utilities for applying transformations to elements
 * 
 * Provides projection helpers for algorithms that need to transform/access
 * elements. Supports member pointers, callable objects, and composition.
 * 
 * Ported from Unreal Engine's Templates/Projection.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/Invoke.h"
#include <type_traits>

namespace OloEngine
{
    // ========================================================================
    // Private Implementation
    // ========================================================================

    namespace Private
    {
        /**
         * @brief Helper for member function pointers
         */
        template <typename InvocableType>
        struct TProjectionMemberFunction;

        template <typename ClassType, typename FunctionType>
        struct TProjectionMemberFunction<FunctionType ClassType::*>
        {
            FunctionType ClassType::* MemberFunctionPtr;

            template <typename Arg0Type, typename... ArgTypes>
            constexpr decltype(auto) operator()(Arg0Type&& Arg0, ArgTypes&&... Args) const
            {
                using DecayedArg0Type = std::decay_t<Arg0Type>;
                if constexpr (std::is_base_of_v<ClassType, DecayedArg0Type>)
                {
                    return (((Arg0Type&&)Arg0).*this->MemberFunctionPtr)((ArgTypes&&)Args...);
                }
                else
                {
                    return ((*(Arg0Type&&)Arg0).*this->MemberFunctionPtr)((ArgTypes&&)Args...);
                }
            }
        };

        /**
         * @brief Helper for member data pointers
         */
        template <typename InvocableType>
        struct TProjectionMemberData;

        template <typename ClassType, typename MemberType>
        struct TProjectionMemberData<MemberType ClassType::*>
        {
            MemberType ClassType::* DataMemberPtr;

            template <typename Arg0Type>
            constexpr decltype(auto) operator()(Arg0Type&& Arg0) const
            {
                using DecayedArg0Type = std::decay_t<Arg0Type>;
                if constexpr (std::is_base_of_v<ClassType, DecayedArg0Type>)
                {
                    return ((Arg0Type&&)Arg0).*this->DataMemberPtr;
                }
                else
                {
                    return (*(Arg0Type&&)Arg0).*this->DataMemberPtr;
                }
            }
        };

        /**
         * @brief Check if a member pointer points to a function
         */
        template <typename Class, typename MemberType>
        inline constexpr bool TIsMemberPointerToFunction(MemberType Class::*)
        {
            return std::is_function_v<MemberType>;
        }
    } // namespace Private

    // ========================================================================
    // Projection Function
    // ========================================================================

    /**
     * @brief Transform an invocable into a callable
     * 
     * Projection() handles member pointers and other invocables, turning them
     * into callable objects that can be used with algorithms like SortBy.
     * 
     * Examples:
     * @code
     * struct FInner { FString Name; };
     * struct FOuter { FInner Inner; };
     * 
     * // Member function pointer
     * Algo::SortBy(Items, Projection(&FOuter::Inner));
     * 
     * // Member data access
     * Algo::SortBy(Items, Projection(&FInner::Name));
     * 
     * // Regular callable - passed through unchanged
     * Algo::SortBy(Items, Projection([](const auto& x) { return x.Priority; }));
     * @endcode
     */
    template <typename Invocable0Type>
    constexpr auto Projection(Invocable0Type&& Invocable0)
    {
        using DecayedInvocable0Type = std::decay_t<Invocable0Type>;
        
        if constexpr (!std::is_member_pointer_v<DecayedInvocable0Type>)
        {
            // Regular callables - just forward
            return (Invocable0Type&&)Invocable0;
        }
        else if constexpr (Private::TIsMemberPointerToFunction(DecayedInvocable0Type{}))
        {
            // Member function pointer
            return Private::TProjectionMemberFunction<DecayedInvocable0Type>{ (Invocable0Type&&)Invocable0 };
        }
        else
        {
            // Member data pointer
            return Private::TProjectionMemberData<DecayedInvocable0Type>{ (Invocable0Type&&)Invocable0 };
        }
    }

} // namespace OloEngine
