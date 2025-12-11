#pragma once

/**
 * @file Projection.h
 * @brief Projection utilities for applying transformations to elements
 * 
 * Ported from Unreal Engine's Templates/Projection.h
 */

#include "OloEngine/Core/Base.h"
#include <functional>

namespace OloEngine
{
    /**
     * A projection wrapper that applies a transformation to an element.
     * For simple cases (identity functor), just passes through.
     */
    template <typename ProjectionType>
    OLO_FINLINE constexpr auto Projection(ProjectionType&& Proj) -> ProjectionType&&
    {
        return std::forward<ProjectionType>(Proj);
    }

} // namespace OloEngine
