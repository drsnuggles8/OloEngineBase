#include "OloEnginePCH.h"
#include "OloEngine/Tilemap/Tileset.h"

namespace OloEngine
{
    namespace
    {
        // Tiles that fit along one axis of `extent` pixels given the Tiled-style
        // margin/spacing layout: margin + n*tile + (n-1)*spacing <= extent.
        // Returns 0 rather than a negative count when the atlas is too small.
        u32 AxisCount(u32 extent, u32 tile, u32 spacing, u32 margin)
        {
            if (tile == 0 || extent <= margin)
                return 0;
            const u32 usable = extent - margin;
            if (usable < tile)
                return 0;
            // The last tile has no trailing spacing, so add one span back before dividing.
            return (usable + spacing) / (tile + spacing);
        }
    } // namespace

    u32 Tileset::GetColumnsFor(u32 textureWidth) const
    {
        return AxisCount(textureWidth, m_TileWidth, m_Spacing, m_Margin);
    }

    u32 Tileset::GetRowsFor(u32 textureHeight) const
    {
        return AxisCount(textureHeight, m_TileHeight, m_Spacing, m_Margin);
    }

    u32 Tileset::GetColumns() const
    {
        return GetColumnsFor(m_TextureWidth);
    }

    u32 Tileset::GetRows() const
    {
        return GetRowsFor(m_TextureHeight);
    }

    bool Tileset::GetTileUV(u32 tileIndex, glm::vec2& uvMin, glm::vec2& uvMax) const
    {
        return GetTileUVForAtlas(tileIndex, m_TextureWidth, m_TextureHeight, uvMin, uvMax);
    }

    bool Tileset::GetTileUVForAtlas(u32 tileIndex, u32 textureWidth, u32 textureHeight,
                                    glm::vec2& uvMin, glm::vec2& uvMax) const
    {
        const u32 columns = GetColumnsFor(textureWidth);
        const u32 rows = GetRowsFor(textureHeight);
        if (columns == 0 || rows == 0 || tileIndex >= columns * rows)
            return false;

        const u32 col = tileIndex % columns;
        const u32 row = tileIndex / columns;

        const f32 x0 = static_cast<f32>(m_Margin + col * (m_TileWidth + m_Spacing));
        const f32 y0 = static_cast<f32>(m_Margin + row * (m_TileHeight + m_Spacing));
        const f32 x1 = x0 + static_cast<f32>(m_TileWidth);
        const f32 y1 = y0 + static_cast<f32>(m_TileHeight);

        const f32 texW = static_cast<f32>(textureWidth);
        const f32 texH = static_cast<f32>(textureHeight);

        // Index 0 is the top-left tile in image space; GL's V axis runs bottom-up,
        // so the row range is mirrored on the way out.
        uvMin = { x0 / texW, 1.0f - (y1 / texH) };
        uvMax = { x1 / texW, 1.0f - (y0 / texH) };
        return true;
    }

    void Tileset::SetTileInfo(u32 tileIndex, const TileInfo& info)
    {
        if (tileIndex >= m_Tiles.size())
            m_Tiles.resize(static_cast<sizet>(tileIndex) + 1);
        m_Tiles[tileIndex] = info;
    }
} // namespace OloEngine
