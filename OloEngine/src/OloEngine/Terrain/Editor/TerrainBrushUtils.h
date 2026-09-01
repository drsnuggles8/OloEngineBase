#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OloEngine
{
    // Shared brush utilities used by TerrainBrush and TerrainPaintBrush
    namespace TerrainBrushUtils
    {
        inline f32 ComputeFalloff(f32 distance, f32 radius, f32 falloff)
        {
            if (radius <= 0.0f || distance >= radius)
                return 0.0f;

            falloff = std::clamp(falloff, 0.0f, 1.0f);
            f32 t = distance / radius; // [0, 1]

            // Blend between hard (constant 1) and smooth (cosine) based on falloff
            f32 smooth = 0.5f * (1.0f + std::cos(t * std::numbers::pi_v<f32>));
            return std::lerp(1.0f, smooth, falloff);
        }

        // Texel-space bounding rect of a world-space brush, in the [0, resolution)
        // coordinates of a square authoring texture.
        //
        // Shared by the CPU brushes and the GPU dispatch (issue #716) so the two
        // cannot disagree about which texels a stroke covers. A GPU brush that
        // derived its own rect would look identical until the terrain stopped
        // being square, at which point the two would diverge along one axis only
        // — the space-mismatch failure in
        // docs/agent-rules/cpu-gpu-surface-parity.md.
        struct BrushRect
        {
            u32 X = 0;
            u32 Y = 0;
            u32 Width = 0;
            u32 Height = 0;

            [[nodiscard]] bool Empty() const
            {
                return Width == 0 || Height == 0;
            }
        };

        inline BrushRect ComputeBrushRect(u32 resolution, f32 normX, f32 normZ, f32 radius,
                                          f32 worldSizeX, f32 worldSizeZ)
        {
            BrushRect rect{};
            if (resolution <= 1 || radius <= 0.0f || worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
                return rect;

            const f32 resF = static_cast<f32>(resolution);
            const f32 radiusTexelsX = (radius / worldSizeX) * resF;
            const f32 radiusTexelsZ = (radius / worldSizeZ) * resF;
            const f32 radiusTexels = std::max(radiusTexelsX, radiusTexelsZ);

            const f32 centerPixelX = normX * (resF - 1.0f);
            const f32 centerPixelZ = normZ * (resF - 1.0f);

            const i32 maxIndex = static_cast<i32>(resolution) - 1;
            const i32 minX = std::max(0, static_cast<i32>(centerPixelX - radiusTexels));
            const i32 maxX = std::min(maxIndex, static_cast<i32>(centerPixelX + radiusTexels));
            const i32 minZ = std::max(0, static_cast<i32>(centerPixelZ - radiusTexels));
            const i32 maxZ = std::min(maxIndex, static_cast<i32>(centerPixelZ + radiusTexels));

            if (minX > maxX || minZ > maxZ)
                return rect;

            rect.X = static_cast<u32>(minX);
            rect.Y = static_cast<u32>(minZ);
            rect.Width = static_cast<u32>(maxX - minX + 1);
            rect.Height = static_cast<u32>(maxZ - minZ + 1);
            return rect;
        }
    } // namespace TerrainBrushUtils
} // namespace OloEngine
