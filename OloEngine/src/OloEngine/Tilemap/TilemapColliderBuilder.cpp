#include "OloEnginePCH.h"
#include "OloEngine/Tilemap/TilemapColliderBuilder.h"

#include "OloEngine/Tilemap/TilemapComponent.h"
#include "OloEngine/Tilemap/Tileset.h"

namespace OloEngine
{
    namespace TilemapCollider
    {
        std::vector<TileColliderRect> MergeSolidRuns(const std::vector<bool>& solid, u32 width, u32 height)
        {
            OLO_PROFILE_FUNCTION();

            std::vector<TileColliderRect> rects;
            if (width == 0 || height == 0)
                return rects;

            const sizet cellCount = static_cast<sizet>(width) * static_cast<sizet>(height);
            // A mask that does not cover the grid is read as non-solid past its end
            // rather than rejected: the same forgiving rule GetTile uses for a short
            // layer, so a truncated scene loses tiles instead of losing collision
            // entirely.
            auto isSolid = [&](u32 x, u32 y)
            {
                const sizet index = static_cast<sizet>(y) * static_cast<sizet>(width) + static_cast<sizet>(x);
                return index < solid.size() && solid[index];
            };

            std::vector<bool> claimed(cellCount, false);
            auto isClaimed = [&](u32 x, u32 y)
            {
                return claimed[static_cast<sizet>(y) * static_cast<sizet>(width) + static_cast<sizet>(x)];
            };

            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    if (!isSolid(x, y) || isClaimed(x, y))
                        continue;

                    // Grow right along the row.
                    u32 runWidth = 1;
                    while (x + runWidth < width && isSolid(x + runWidth, y) && !isClaimed(x + runWidth, y))
                        ++runWidth;

                    // Grow up as long as the whole run stays solid and unclaimed.
                    u32 runHeight = 1;
                    while (y + runHeight < height)
                    {
                        bool rowFits = true;
                        for (u32 dx = 0; dx < runWidth; ++dx)
                        {
                            if (!isSolid(x + dx, y + runHeight) || isClaimed(x + dx, y + runHeight))
                            {
                                rowFits = false;
                                break;
                            }
                        }
                        if (!rowFits)
                            break;
                        ++runHeight;
                    }

                    for (u32 dy = 0; dy < runHeight; ++dy)
                    {
                        for (u32 dx = 0; dx < runWidth; ++dx)
                        {
                            claimed[static_cast<sizet>(y + dy) * static_cast<sizet>(width) + static_cast<sizet>(x + dx)] = true;
                        }
                    }

                    rects.push_back({ x, y, runWidth, runHeight });
                }
            }

            return rects;
        }

        std::vector<bool> BuildSolidMask(const TilemapComponent& tilemap, const Ref<Tileset>& tileset)
        {
            OLO_PROFILE_FUNCTION();

            const sizet cellCount = static_cast<sizet>(tilemap.Width) * static_cast<sizet>(tilemap.Height);
            std::vector<bool> mask(cellCount, false);
            if (!tileset)
                return mask;

            for (sizet layerIndex = 0; layerIndex < tilemap.Layers.size(); ++layerIndex)
            {
                if (!tilemap.Layers[layerIndex].Solid)
                    continue;
                for (u32 y = 0; y < tilemap.Height; ++y)
                {
                    for (u32 x = 0; x < tilemap.Width; ++x)
                    {
                        const u32 entry = tilemap.GetTile(layerIndex, x, y);
                        if (entry == TilemapComponent::kEmptyTile)
                            continue;
                        if (tileset->IsTileSolid(entry - 1))
                            mask[tilemap.CellIndex(x, y)] = true;
                    }
                }
            }
            return mask;
        }

        std::vector<TileColliderRect> BuildColliderRects(const TilemapComponent& tilemap, const Ref<Tileset>& tileset)
        {
            return MergeSolidRuns(BuildSolidMask(tilemap, tileset), tilemap.Width, tilemap.Height);
        }
    } // namespace TilemapCollider
} // namespace OloEngine
