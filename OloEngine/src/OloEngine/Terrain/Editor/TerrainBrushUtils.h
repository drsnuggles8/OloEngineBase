#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Terrain/TerrainData.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OloEngine
{
    // The editor preview is a cylinder around the hit's world XZ. On tilted
    // terrain the sample's height also moves it horizontally across that circle.
    struct TerrainBrushProjection
    {
        glm::mat2 GridAxes{ 1.0f };
        glm::vec2 HeightAxis{ 0.0f };   // world XZ per unit normalized height
        glm::vec2 HeightOffset{ 0.0f }; // subtract the hit's own horizontal height displacement
        const TerrainData* Heights = nullptr;

        [[nodiscard]] static TerrainBrushProjection FromTerrain(const glm::mat4& model, f32 localHitY, f32 heightScale, const TerrainData& data)
        {
            const glm::vec2 up{ model[1].x, model[1].z };
            return { glm::mat2({ model[0].x, model[0].z }, { model[2].x, model[2].z }),
                     up * heightScale, -up * localHitY, &data };
        }

        [[nodiscard]] bool UsesHeight() const
        {
            return std::abs(HeightAxis.x) > 0.0f || std::abs(HeightAxis.y) > 0.0f;
        }

        [[nodiscard]] f32 Distance(glm::vec2 localDelta, f32 height) const
        {
            return glm::length(GridAxes * localDelta + HeightAxis * height + HeightOffset);
        }
    };

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

        enum class Grid
        {
            HeightSamples,
            SplatTexels
        };

        inline BrushRect ComputeBrushRect(u32 resolution, f32 normX, f32 normZ, f32 radius,
                                          f32 worldSizeX, f32 worldSizeZ, const TerrainBrushProjection& projection = {},
                                          Grid grid = Grid::HeightSamples)
        {
            BrushRect rect{};
            if (resolution <= 1 || !(radius > 0.0f) || !(worldSizeX > 0.0f) || !(worldSizeZ > 0.0f) ||
                !std::isfinite(normX) || !std::isfinite(normZ) || (projection.UsesHeight() && !projection.Heights))
                return rect;

            // Each row of the inverse projected basis gives the extent of the
            // world circle along one grid axis. A height variation also shifts
            // its centre. Bound the two endpoints of the loaded height range.
            // A vertical base plane or an unsynchronized height range requires
            // a conservative full dispatch; the per-sample circle stays exact.
            const glm::mat2 inverse = glm::inverse(projection.GridAxes);
            const glm::vec2 radii = radius * glm::vec2(std::hypot(inverse[0].x, inverse[1].x),
                                                       std::hypot(inverse[0].y, inverse[1].y));
            if (!std::isfinite(radii.x) || !std::isfinite(radii.y))
                return { 0, 0, resolution, resolution };
            glm::vec2 lo = -radii - inverse * projection.HeightOffset;
            glm::vec2 hi = radii - inverse * projection.HeightOffset;
            if (projection.UsesHeight())
            {
                const auto range = projection.Heights->GetHeightRange();
                if (!range)
                    return { 0, 0, resolution, resolution };
                const glm::vec2 shift = -(inverse * projection.HeightAxis);
                lo += glm::min(shift * range->x, shift * range->y);
                hi += glm::max(shift * range->x, shift * range->y);
            }
            const glm::vec2 size{ worldSizeX, worldSizeZ }, center{ normX, normZ };
            const f32 last = static_cast<f32>(resolution - 1u);
            const bool splat = grid == Grid::SplatTexels;
            const f32 intervals = splat ? static_cast<f32>(resolution) : last;
            const f32 texelOffset = splat ? 0.5f : 0.0f;
            lo = glm::max(glm::floor((center + lo / size) * intervals - texelOffset), glm::vec2(0));
            hi = glm::min(glm::ceil((center + hi / size) * intervals - texelOffset), glm::vec2(last));
            if (!(lo.x <= hi.x) || !(lo.y <= hi.y))
                return rect;
            rect.X = static_cast<u32>(lo.x);
            rect.Y = static_cast<u32>(lo.y);
            rect.Width = static_cast<u32>(hi.x) - rect.X + 1;
            rect.Height = static_cast<u32>(hi.y) - rect.Y + 1;
            return rect;
        }
    } // namespace TerrainBrushUtils
} // namespace OloEngine
