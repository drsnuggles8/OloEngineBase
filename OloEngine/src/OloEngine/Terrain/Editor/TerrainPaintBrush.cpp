#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Editor/TerrainPaintBrush.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainLayer.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    f32 TerrainPaintBrush::ComputeFalloff(f32 distance, f32 radius, f32 falloff)
    {
        if (distance >= radius)
            return 0.0f;

        f32 t = distance / radius;
        f32 smooth = 0.5f * (1.0f + std::cos(t * glm::pi<f32>()));
        return glm::mix(1.0f, smooth, falloff);
    }

    TerrainPaintBrush::DirtyRegion TerrainPaintBrush::Apply(
        TerrainMaterial& material,
        const TerrainPaintSettings& settings,
        const glm::vec3& worldPos,
        f32 worldSizeX, f32 worldSizeZ,
        f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        DirtyRegion dirty{};

        if (!material.HasCPUSplatmaps() || settings.TargetLayer >= material.GetLayerCount())
            return dirty;

        u32 res = material.GetSplatmapResolution();
        u32 splatmapIdx = settings.TargetLayer / 4; // 0 for layers 0-3, 1 for layers 4-7
        u32 channelIdx = settings.TargetLayer % 4;  // R=0, G=1, B=2, A=3

        auto& splatData = material.GetSplatmapData(splatmapIdx);

        // Also need access to the other splatmap for normalization
        auto& splatData0 = material.GetSplatmapData(0);
        auto& splatData1 = material.GetSplatmapData(1);
        u32 layerCount = material.GetLayerCount();

        // Convert world position to splatmap texel coords
        f32 normX = worldPos.x / worldSizeX;
        f32 normZ = worldPos.z / worldSizeZ;

        f32 radiusTexelsX = (settings.Radius / worldSizeX) * static_cast<f32>(res);
        f32 radiusTexelsZ = (settings.Radius / worldSizeZ) * static_cast<f32>(res);
        f32 radiusTexels = std::max(radiusTexelsX, radiusTexelsZ);

        f32 centerPixelX = normX * static_cast<f32>(res - 1);
        f32 centerPixelZ = normZ * static_cast<f32>(res - 1);

        i32 minX = std::max(0, static_cast<i32>(centerPixelX - radiusTexels));
        i32 maxX = std::min(static_cast<i32>(res - 1), static_cast<i32>(centerPixelX + radiusTexels));
        i32 minZ = std::max(0, static_cast<i32>(centerPixelZ - radiusTexels));
        i32 maxZ = std::min(static_cast<i32>(res - 1), static_cast<i32>(centerPixelZ + radiusTexels));

        if (minX > maxX || minZ > maxZ)
            return dirty;

        f32 strengthDt = settings.Strength * deltaTime;

        for (i32 z = minZ; z <= maxZ; ++z)
        {
            for (i32 x = minX; x <= maxX; ++x)
            {
                f32 dx = (static_cast<f32>(x) / static_cast<f32>(res - 1) - normX) * worldSizeX;
                f32 dz = (static_cast<f32>(z) / static_cast<f32>(res - 1) - normZ) * worldSizeZ;
                f32 dist = std::sqrt(dx * dx + dz * dz);

                if (dist > settings.Radius)
                    continue;

                f32 weight = ComputeFalloff(dist, settings.Radius, settings.Falloff);
                f32 addAmount = weight * strengthDt * 255.0f;

                sizet pixelIdx = (static_cast<sizet>(z) * res + static_cast<sizet>(x)) * 4;

                // Read current target channel value
                f32 current = static_cast<f32>(splatData[pixelIdx + channelIdx]);
                f32 newVal = std::min(current + addAmount, 255.0f);
                splatData[pixelIdx + channelIdx] = static_cast<u8>(newVal);

                // Normalize: all 8 channels across both splatmaps should sum to 255
                // Gather total weight
                f32 total = 0.0f;
                for (u32 ch = 0; ch < 4; ++ch)
                {
                    total += static_cast<f32>(splatData0[pixelIdx + ch]);
                }
                if (layerCount > 4)
                {
                    for (u32 ch = 0; ch < 4; ++ch)
                    {
                        total += static_cast<f32>(splatData1[pixelIdx + ch]);
                    }
                }

                // Normalize all channels so they sum to 255
                if (total > 0.0f)
                {
                    f32 scale = 255.0f / total;
                    for (u32 ch = 0; ch < 4; ++ch)
                    {
                        splatData0[pixelIdx + ch] = static_cast<u8>(
                            std::min(static_cast<f32>(splatData0[pixelIdx + ch]) * scale, 255.0f));
                    }
                    if (layerCount > 4)
                    {
                        for (u32 ch = 0; ch < 4; ++ch)
                        {
                            splatData1[pixelIdx + ch] = static_cast<u8>(
                                std::min(static_cast<f32>(splatData1[pixelIdx + ch]) * scale, 255.0f));
                        }
                    }
                }
            }
        }

        dirty.X = static_cast<u32>(minX);
        dirty.Y = static_cast<u32>(minZ);
        dirty.Width = static_cast<u32>(maxX - minX + 1);
        dirty.Height = static_cast<u32>(maxZ - minZ + 1);
        return dirty;
    }
} // namespace OloEngine
