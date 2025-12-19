#pragma once

// @file IdentityFunctor.h
// @brief A functor which returns whatever is passed to it
// 
// Ported from Unreal Engine's Templates/IdentityFunctor.h

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // @brief A functor which returns whatever is passed to it. Mainly used for generic composition.
    struct FIdentityFunctor
    {
        template <typename T>
        OLO_FINLINE constexpr T&& operator()(T&& Val) const
        {
            return static_cast<T&&>(Val);
        }
    };

} // namespace OloEngine
