#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class TerrainMaterial;

    struct TerrainPaintSettings
    {
        f32 Radius = 10.0f;  // World-space radius
        f32 Strength = 0.5f; // Paint strength per application [0, 1]
        f32 Falloff = 0.5f;  // 0 = hard brush, 1 = soft falloff
        u32 TargetLayer = 0; // Which layer to paint (0-7)
    };

    // Splatmap painting brush. Paints on CPU splatmap data and marks dirty regions.
    class TerrainPaintBrush
    {
      public:
        struct DirtyRegion
        {
            u32 X = 0;
            u32 Y = 0;
            u32 Width = 0;
            u32 Height = 0;
        };

        // Apply paint at a world-space position on the splatmap(s).
        // Works on CPU splatmap data stored in TerrainMaterial.
        static DirtyRegion Apply(
            TerrainMaterial& material,
            const TerrainPaintSettings& settings,
            const glm::vec3& worldPos,
            f32 worldSizeX, f32 worldSizeZ,
            f32 deltaTime);
    };
} // namespace OloEngine
