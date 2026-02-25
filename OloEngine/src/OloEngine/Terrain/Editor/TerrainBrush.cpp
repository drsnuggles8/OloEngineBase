#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Editor/TerrainBrush.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainChunk.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    f32 TerrainBrush::ComputeFalloff(f32 distance, f32 radius, f32 falloff)
    {
        if (distance >= radius)
            return 0.0f;

        f32 t = distance / radius; // [0, 1]

        // Blend between hard (constant 1) and smooth (cosine) based on falloff
        f32 smooth = 0.5f * (1.0f + std::cos(t * glm::pi<f32>()));
        return glm::mix(1.0f, smooth, falloff);
    }

    TerrainBrush::DirtyRegion TerrainBrush::Apply(
        TerrainData& terrainData,
        const TerrainBrushSettings& settings,
        const glm::vec3& worldPos,
        f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
        f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        DirtyRegion dirty{};
        auto& heights = terrainData.GetHeightData();
        u32 res = terrainData.GetResolution();

        if (res <= 1 || heights.empty() || heightScale <= 0.0f || worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
            return dirty;

        // Convert world position to normalized terrain coords [0, 1]
        f32 normX = worldPos.x / worldSizeX;
        f32 normZ = worldPos.z / worldSizeZ;

        // Convert radius to texel space
        f32 radiusTexelsX = (settings.Radius / worldSizeX) * static_cast<f32>(res);
        f32 radiusTexelsZ = (settings.Radius / worldSizeZ) * static_cast<f32>(res);
        f32 radiusTexels = std::max(radiusTexelsX, radiusTexelsZ);

        // Compute pixel bounds
        f32 centerPixelX = normX * static_cast<f32>(res - 1);
        f32 centerPixelZ = normZ * static_cast<f32>(res - 1);

        i32 minX = std::max(0, static_cast<i32>(centerPixelX - radiusTexels));
        i32 maxX = std::min(static_cast<i32>(res - 1), static_cast<i32>(centerPixelX + radiusTexels));
        i32 minZ = std::max(0, static_cast<i32>(centerPixelZ - radiusTexels));
        i32 maxZ = std::min(static_cast<i32>(res - 1), static_cast<i32>(centerPixelZ + radiusTexels));

        if (minX > maxX || minZ > maxZ)
            return dirty;

        // Pre-compute target height for Flatten/Level tools
        f32 targetHeight = 0.0f;
        if (settings.Tool == TerrainBrushTool::Flatten || settings.Tool == TerrainBrushTool::Level)
        {
            // Use height at brush center as target
            targetHeight = terrainData.GetHeightAt(normX, normZ);
        }

        f32 strengthDt = settings.Strength * deltaTime;
        f32 invHeightScale = 1.0f / heightScale;
        bool changed = false;

        for (i32 z = minZ; z <= maxZ; ++z)
        {
            for (i32 x = minX; x <= maxX; ++x)
            {
                // Distance in world space
                f32 dx = (static_cast<f32>(x) / static_cast<f32>(res - 1) - normX) * worldSizeX;
                f32 dz = (static_cast<f32>(z) / static_cast<f32>(res - 1) - normZ) * worldSizeZ;
                f32 dist = std::sqrt(dx * dx + dz * dz);

                if (dist > settings.Radius)
                    continue;

                f32 weight = ComputeFalloff(dist, settings.Radius, settings.Falloff);
                f32 influence = glm::clamp(weight * strengthDt, 0.0f, 1.0f);
                sizet idx = static_cast<sizet>(z) * res + static_cast<sizet>(x);

                switch (settings.Tool)
                {
                    case TerrainBrushTool::Raise:
                        heights[idx] += influence * invHeightScale;
                        break;

                    case TerrainBrushTool::Lower:
                        heights[idx] -= influence * invHeightScale;
                        break;

                    case TerrainBrushTool::Smooth:
                    {
                        // Average of 4 neighbors
                        f32 avg = 0.0f;
                        i32 count = 0;
                        if (x > 0)
                        {
                            avg += heights[idx - 1];
                            ++count;
                        }
                        if (x < static_cast<i32>(res) - 1)
                        {
                            avg += heights[idx + 1];
                            ++count;
                        }
                        if (z > 0)
                        {
                            avg += heights[idx - res];
                            ++count;
                        }
                        if (z < static_cast<i32>(res) - 1)
                        {
                            avg += heights[idx + res];
                            ++count;
                        }
                        if (count > 0)
                        {
                            avg /= static_cast<f32>(count);
                            heights[idx] += (avg - heights[idx]) * influence;
                        }
                        break;
                    }

                    case TerrainBrushTool::Flatten:
                        heights[idx] = glm::mix(heights[idx], targetHeight, influence);
                        break;

                    case TerrainBrushTool::Level:
                    {
                        // Move towards target height — same as flatten but from initial click height
                        heights[idx] = glm::mix(heights[idx], targetHeight, influence);
                        break;
                    }
                }

                heights[idx] = std::clamp(heights[idx], 0.0f, 1.0f);
                changed = true;
            }
        }

        if (!changed)
            return dirty;

        dirty.X = static_cast<u32>(minX);
        dirty.Y = static_cast<u32>(minZ);
        dirty.Width = static_cast<u32>(maxX - minX + 1);
        dirty.Height = static_cast<u32>(maxZ - minZ + 1);
        return dirty;
    }

    void TerrainBrush::RebuildDirtyChunks(
        TerrainChunkManager& chunkManager,
        const TerrainData& terrainData,
        const DirtyRegion& region,
        f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        if (region.Width == 0 || region.Height == 0)
            return;

        u32 res = terrainData.GetResolution();
        u32 numChunksX = chunkManager.GetNumChunksX();
        u32 numChunksZ = chunkManager.GetNumChunksZ();

        if (numChunksX == 0 || numChunksZ == 0)
            return;

        u32 chunkRes = TerrainChunk::CHUNK_RESOLUTION;

        // Find which chunks overlap the dirty region
        u32 startChunkX = region.X / chunkRes;
        u32 endChunkX = std::min((region.X + region.Width - 1) / chunkRes, numChunksX - 1);
        u32 startChunkZ = region.Y / chunkRes;
        u32 endChunkZ = std::min((region.Y + region.Height - 1) / chunkRes, numChunksZ - 1);

        for (u32 cz = startChunkZ; cz <= endChunkZ; ++cz)
        {
            for (u32 cx = startChunkX; cx <= endChunkX; ++cx)
            {
                chunkManager.RebuildChunk(terrainData, cx, cz, worldSizeX, worldSizeZ, heightScale);
            }
        }
    }
} // namespace OloEngine
