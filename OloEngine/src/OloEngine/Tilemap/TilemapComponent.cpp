#include "OloEnginePCH.h"
#include "OloEngine/Tilemap/TilemapComponent.h"

namespace OloEngine
{
    void TilemapComponent::Resize(u32 newWidth, u32 newHeight)
    {
        // Bounded on BOTH ends. Lua exposes Resize directly, so an unbounded upper
        // limit turns `tilemap:resize(100000, 100000)` into a length_error/bad_alloc
        // rather than a refused call.
        if (newWidth == 0 || newHeight == 0 || newWidth > kMaxExtent || newHeight > kMaxExtent)
        {
            OLO_CORE_WARN("TilemapComponent::Resize - refusing a {}x{} grid; the extent must be within [1, {}].",
                          newWidth, newHeight, kMaxExtent);
            return;
        }
        if (newWidth == Width && newHeight == Height)
            return;

        const sizet newCount = static_cast<sizet>(newWidth) * static_cast<sizet>(newHeight);
        const u32 copyWidth = std::min(Width, newWidth);
        const u32 copyHeight = std::min(Height, newHeight);

        for (auto& layer : Layers)
        {
            std::vector<u32> resized(newCount, kEmptyTile);
            for (u32 y = 0; y < copyHeight; ++y)
            {
                for (u32 x = 0; x < copyWidth; ++x)
                {
                    const sizet oldIndex = static_cast<sizet>(y) * static_cast<sizet>(Width) + static_cast<sizet>(x);
                    if (oldIndex < layer.Tiles.size())
                        resized[static_cast<sizet>(y) * static_cast<sizet>(newWidth) + static_cast<sizet>(x)] = layer.Tiles[oldIndex];
                }
            }
            layer.Tiles = std::move(resized);
        }

        Width = newWidth;
        Height = newHeight;
    }
} // namespace OloEngine
