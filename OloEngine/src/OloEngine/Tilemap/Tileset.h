#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace OloEngine
{
    // @brief Per-tile authored metadata inside a Tileset.
    //
    // One record per tile index in the atlas. `Solid` is what the collider
    // generator reads; `Flags` is a free user bitfield (one-way platform,
    // damage, water, …) that gameplay code interprets. Neither is consulted by
    // the renderer — a tile draws the same whether it is solid or not.
    struct TileInfo
    {
        bool Solid = false;
        u32 Flags = 0;
        // Free-form authoring label ("grass", "ladder"). Not used by the engine.
        std::string Type;
    };

    // @brief A texture atlas sliced into a uniform grid of tiles, plus per-tile metadata.
    //
    // The atlas is addressed by a zero-based *tile index* running left-to-right,
    // top-to-bottom. `TilemapComponent` stores indices biased by one so that 0
    // can mean "empty" in the layer arrays; `Tileset` itself always speaks
    // unbiased indices.
    //
    // Slicing follows the Tiled convention: `Margin` pixels of border around the
    // whole image, `Spacing` pixels between adjacent tiles. Column and row counts
    // are derived from the texture size, so a re-exported atlas of a different
    // size re-slices without re-authoring — which is also why the counts are not
    // serialized.
    class Tileset : public Asset
    {
      public:
        Tileset() = default;
        ~Tileset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::Tileset;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        AssetHandle GetTextureHandle() const
        {
            return m_TextureHandle;
        }
        void SetTextureHandle(AssetHandle handle)
        {
            m_TextureHandle = handle;
        }

        u32 GetTileWidth() const
        {
            return m_TileWidth;
        }
        u32 GetTileHeight() const
        {
            return m_TileHeight;
        }
        u32 GetSpacing() const
        {
            return m_Spacing;
        }
        u32 GetMargin() const
        {
            return m_Margin;
        }

        // Pixel dimensions of the atlas image — an OPTIONAL cached hint, zero until
        // someone who already holds the texture fills it in (the editor's tileset
        // picker does; tests set it directly).
        //
        // Deliberately NOT resolved by TilesetSerializer::TryLoadData. Calling
        // AssetManager::GetAsset there re-enters AssetImporter::TryLoadData, whose
        // registry mutex is not recursive, and the editor deadlocks on the first
        // frame that draws a tilemap. The render path does not need this field at
        // all — it slices via GetTileUVForAtlas against the texture it is about to
        // sample, which is also correct when the atlas was re-exported at a new size.
        u32 GetTextureWidth() const
        {
            return m_TextureWidth;
        }
        u32 GetTextureHeight() const
        {
            return m_TextureHeight;
        }

        void SetTileSize(u32 width, u32 height)
        {
            m_TileWidth = width;
            m_TileHeight = height;
        }
        void SetSpacing(u32 spacing)
        {
            m_Spacing = spacing;
        }
        void SetMargin(u32 margin)
        {
            m_Margin = margin;
        }
        void SetTextureSize(u32 width, u32 height)
        {
            m_TextureWidth = width;
            m_TextureHeight = height;
        }

        // Tiles per row / per column, derived from the atlas size. Zero when the
        // texture size is unknown or the tile size is degenerate — GetTileUV
        // then reports failure rather than dividing by zero.
        [[nodiscard]] u32 GetColumns() const;
        [[nodiscard]] u32 GetRows() const;

        // The same counts for an atlas of an explicitly supplied pixel size,
        // ignoring the cached one. The renderer uses these so it can slice
        // against the texture it is about to sample without writing the size
        // back into a shared asset mid-frame.
        [[nodiscard]] u32 GetColumnsFor(u32 textureWidth) const;
        [[nodiscard]] u32 GetRowsFor(u32 textureHeight) const;

        // Total addressable tile indices (Columns * Rows).
        [[nodiscard]] u32 GetTileCount() const
        {
            return GetColumns() * GetRows();
        }

        // @brief UV rect of one tile, in the [0,1] texture space Renderer2D expects.
        //
        // `uvMin` is the bottom-left corner and `uvMax` the top-right, matching
        // Renderer2D::DrawQuad's uvMin/uvMax overload. Tile index 0 is the
        // *top-left* tile of the atlas (image convention), so the V axis is
        // flipped on the way out (GL convention).
        //
        // @return false — leaving uvMin/uvMax untouched — when the index is out
        //         of range or the atlas geometry is not yet known.
        [[nodiscard]] bool GetTileUV(u32 tileIndex, glm::vec2& uvMin, glm::vec2& uvMax) const;

        // GetTileUV against an explicitly supplied atlas size. Same contract,
        // but it reads nothing the caller has not passed in — which is what lets
        // the renderer slice against the live texture without mutating the
        // Tileset (a `const Ref<Tileset>&` is const-propagating, and a shared
        // asset should not be written to from inside a draw anyway).
        [[nodiscard]] bool GetTileUVForAtlas(u32 tileIndex, u32 textureWidth, u32 textureHeight,
                                             glm::vec2& uvMin, glm::vec2& uvMax) const;

        // Per-tile metadata. The vector is sparse in the sense that it may be
        // shorter than GetTileCount(); a missing entry reads as a default
        // TileInfo (non-solid, no flags).
        const std::vector<TileInfo>& GetTiles() const
        {
            return m_Tiles;
        }
        std::vector<TileInfo>& GetTiles()
        {
            return m_Tiles;
        }

        [[nodiscard]] TileInfo GetTileInfo(u32 tileIndex) const
        {
            if (tileIndex < m_Tiles.size())
                return m_Tiles[tileIndex];
            return {};
        }

        [[nodiscard]] bool IsTileSolid(u32 tileIndex) const
        {
            return tileIndex < m_Tiles.size() && m_Tiles[tileIndex].Solid;
        }

        // Grows the metadata array so `tileIndex` is addressable, then writes it.
        void SetTileInfo(u32 tileIndex, const TileInfo& info);

      private:
        AssetHandle m_TextureHandle = 0;
        u32 m_TileWidth = 16;
        u32 m_TileHeight = 16;
        u32 m_Spacing = 0;
        u32 m_Margin = 0;
        u32 m_TextureWidth = 0;
        u32 m_TextureHeight = 0;
        std::vector<TileInfo> m_Tiles;
    };
} // namespace OloEngine
