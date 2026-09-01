#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Editor/TerrainPaintBrush.h"
#include "OloEngine/Terrain/Editor/TerrainBrushUtils.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainLayer.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
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
        if (res <= 1 || worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
            return dirty;
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

        // Shared with TerrainGPUBrush (issue #716) — see TerrainBrush::Apply.
        const auto rect = TerrainBrushUtils::ComputeBrushRect(res, normX, normZ, settings.Radius,
                                                              worldSizeX, worldSizeZ);
        if (rect.Empty())
            return dirty;

        const i32 minX = static_cast<i32>(rect.X);
        const i32 maxX = static_cast<i32>(rect.X + rect.Width) - 1;
        const i32 minZ = static_cast<i32>(rect.Y);
        const i32 maxZ = static_cast<i32>(rect.Y + rect.Height) - 1;

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

                f32 weight = TerrainBrushUtils::ComputeFalloff(dist, settings.Radius, settings.Falloff);
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

        dirty.X = rect.X;
        dirty.Y = rect.Y;
        dirty.Width = rect.Width;
        dirty.Height = rect.Height;
        return dirty;
    }
} // namespace OloEngine
