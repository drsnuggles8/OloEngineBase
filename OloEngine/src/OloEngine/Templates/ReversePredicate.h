#pragma once

// @file ReversePredicate.h
// @brief Wrapper for reversing a predicate
// 
// Ported from Unreal Engine's Templates/ReversePredicate.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/Invoke.h"

namespace OloEngine
{
    // @brief Wrapper for reversing a binary predicate.
    // Returns the result of invoking the predicate with arguments swapped.
    template <typename PredicateType>
    class TReversePredicate
    {
        const PredicateType& Predicate;

    public:
        TReversePredicate(const PredicateType& InPredicate)
            : Predicate(InPredicate)
        {
        }

        template <typename T>
        OLO_FINLINE bool operator()(T&& A, T&& B) const
        {
            return Invoke(Predicate, Forward<T>(B), Forward<T>(A));
        }
    };

} // namespace OloEngine
