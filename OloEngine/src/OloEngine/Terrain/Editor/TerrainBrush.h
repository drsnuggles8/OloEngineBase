#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class TerrainData;
    class TerrainChunkManager;

    enum class TerrainBrushTool : u8
    {
        Raise = 0,
        Lower,
        Smooth,
        Flatten,
        Level
    };

    struct TerrainBrushSettings
    {
        f32 Radius = 10.0f;  // World-space radius
        f32 Strength = 0.5f; // Effect strength per application [0, 1]
        f32 Falloff = 0.5f;  // 0 = no falloff (hard), 1 = full falloff (soft)
        TerrainBrushTool Tool = TerrainBrushTool::Raise;
    };

    // Heightmap sculpting brush. Operates on CPU data, marks dirty regions for GPU upload.
    class TerrainBrush
    {
      public:
        // Apply the brush at a world-space position.
        // Returns the dirty heightmap region (pixel coords) for partial GPU re-upload.
        struct DirtyRegion
        {
            u32 X = 0;
            u32 Y = 0;
            u32 Width = 0;
            u32 Height = 0;
        };

        static DirtyRegion Apply(
            TerrainData& terrainData,
            const TerrainBrushSettings& settings,
            const glm::vec3& worldPos,
            f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
            f32 deltaTime);

        // Rebuild only the chunks affected by the dirty region
        static void RebuildDirtyChunks(
            TerrainChunkManager& chunkManager,
            const TerrainData& terrainData,
            const DirtyRegion& region,
            f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

      private:
        // Compute falloff weight at distance d from center, given radius and falloff
        static f32 ComputeFalloff(f32 distance, f32 radius, f32 falloff);
    };
} // namespace OloEngine
