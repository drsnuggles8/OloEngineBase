#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <cmath>

namespace OloEngine
{
    // Shared brush utilities used by TerrainBrush and TerrainPaintBrush
    namespace TerrainBrushUtils
    {
        inline f32 ComputeFalloff(f32 distance, f32 radius, f32 falloff)
        {
            if (distance >= radius)
                return 0.0f;

            f32 t = distance / radius; // [0, 1]

            // Blend between hard (constant 1) and smooth (cosine) based on falloff
            f32 smooth = 0.5f * (1.0f + std::cos(t * glm::pi<f32>()));
            return glm::mix(1.0f, smooth, falloff);
        }
    } // namespace TerrainBrushUtils
} // namespace OloEngine
