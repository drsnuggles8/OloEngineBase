// OLO_TEST_LAYER: unit
// =============================================================================
// TilemapTest.cpp
//
// CPU contracts for the 2D tilemap system (issue #646), in three groups:
//
//   Tileset*        - atlas slicing and UV math. The UV rect is what every tile
//                     draw depends on, and it is pure arithmetic, so it is pinned
//                     here rather than inferred from a rendered frame.
//   TilemapGrid*    - the biased tile encoding and the bounds rules that let a
//                     short or oversized layer be read safely.
//   TilemapCollider* - greedy rectangle merging. The claim being pinned is that
//                     the merge is correct AND that it actually merges: a
//                     one-box-per-tile implementation would pass a naive
//                     "collision exists" check while being useless for Box2D.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Tilemap/TilemapColliderBuilder.h"
#include "OloEngine/Tilemap/TilemapComponent.h"
#include "OloEngine/Tilemap/Tileset.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A 64x64 atlas of 16x16 tiles: 4 columns, 4 rows, 16 tiles.
        Ref<Tileset> MakeTileset(u32 texW = 64, u32 texH = 64, u32 tile = 16, u32 spacing = 0, u32 margin = 0)
        {
            auto tileset = Ref<Tileset>::Create();
            tileset->SetTileSize(tile, tile);
            tileset->SetSpacing(spacing);
            tileset->SetMargin(margin);
            tileset->SetTextureSize(texW, texH);
            return tileset;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Tileset slicing
    // -------------------------------------------------------------------------

    TEST(TilesetSlicing, DerivesColumnsAndRowsFromTheTextureSize)
    {
        auto tileset = MakeTileset();
        EXPECT_EQ(tileset->GetColumns(), 4u);
        EXPECT_EQ(tileset->GetRows(), 4u);
        EXPECT_EQ(tileset->GetTileCount(), 16u);
    }

    TEST(TilesetSlicing, AccountsForMarginAndSpacingLikeTiled)
    {
        // Tiled's layout: margin px of border, spacing px between tiles.
        // 2 + 3*16 + 2*4 = 58 <= 60, but a fourth tile would need 58 + 4 + 16 = 78.
        auto tileset = MakeTileset(/*texW=*/60, /*texH=*/60, /*tile=*/16, /*spacing=*/4, /*margin=*/2);
        EXPECT_EQ(tileset->GetColumns(), 3u);
        EXPECT_EQ(tileset->GetRows(), 3u);
    }

    TEST(TilesetSlicing, ReportsAnEmptyAtlasRatherThanDividingByZero)
    {
        auto tileset = MakeTileset();
        tileset->SetTextureSize(0, 0);
        EXPECT_EQ(tileset->GetColumns(), 0u);
        EXPECT_EQ(tileset->GetRows(), 0u);

        glm::vec2 uvMin{ -1.0f };
        glm::vec2 uvMax{ -1.0f };
        EXPECT_FALSE(tileset->GetTileUV(0, uvMin, uvMax));
        // Failure must leave the outputs untouched, so a caller that ignores the
        // return value draws nothing rather than a garbage rect.
        EXPECT_FLOAT_EQ(uvMin.x, -1.0f);
        EXPECT_FLOAT_EQ(uvMax.x, -1.0f);
    }

    TEST(TilesetSlicing, TileZeroIsTheTopLeftOfTheAtlas)
    {
        auto tileset = MakeTileset();
        glm::vec2 uvMin{};
        glm::vec2 uvMax{};
        ASSERT_TRUE(tileset->GetTileUV(0, uvMin, uvMax));

        // Image-space tile 0 occupies pixels x[0,16) y[0,16) measured from the TOP.
        // In GL's bottom-up UV space that is u[0, 0.25], v[0.75, 1].
        EXPECT_FLOAT_EQ(uvMin.x, 0.0f);
        EXPECT_FLOAT_EQ(uvMax.x, 0.25f);
        EXPECT_FLOAT_EQ(uvMin.y, 0.75f);
        EXPECT_FLOAT_EQ(uvMax.y, 1.0f);
    }

    TEST(TilesetSlicing, IndicesRunLeftToRightThenTopToBottom)
    {
        auto tileset = MakeTileset();
        glm::vec2 min0{};
        glm::vec2 max0{};
        glm::vec2 min1{};
        glm::vec2 max1{};
        glm::vec2 min4{};
        glm::vec2 max4{};
        ASSERT_TRUE(tileset->GetTileUV(0, min0, max0));
        ASSERT_TRUE(tileset->GetTileUV(1, min1, max1));
        ASSERT_TRUE(tileset->GetTileUV(4, min4, max4));

        // Index 1 is one column right: same V band, U shifted by one tile.
        EXPECT_FLOAT_EQ(min1.y, min0.y);
        EXPECT_FLOAT_EQ(min1.x, max0.x);
        // Index 4 wraps to the next row DOWN in image space, i.e. lower V.
        EXPECT_FLOAT_EQ(min4.x, min0.x);
        EXPECT_FLOAT_EQ(max4.y, min0.y);
    }

    TEST(TilesetSlicing, RejectsAnIndexPastTheLastTile)
    {
        auto tileset = MakeTileset();
        glm::vec2 uvMin{};
        glm::vec2 uvMax{};
        EXPECT_TRUE(tileset->GetTileUV(15, uvMin, uvMax));
        EXPECT_FALSE(tileset->GetTileUV(16, uvMin, uvMax));
    }

    TEST(TilesetMetadata, MissingEntriesReadAsNonSolidDefaults)
    {
        auto tileset = MakeTileset();
        EXPECT_FALSE(tileset->IsTileSolid(3));
        EXPECT_FALSE(tileset->GetTileInfo(3).Solid);
        EXPECT_EQ(tileset->GetTileInfo(3).Flags, 0u);

        // Writing a high index grows the array; the gap keeps reading as default.
        TileInfo info;
        info.Solid = true;
        info.Flags = 7;
        tileset->SetTileInfo(5, info);
        EXPECT_TRUE(tileset->IsTileSolid(5));
        EXPECT_EQ(tileset->GetTileInfo(5).Flags, 7u);
        EXPECT_FALSE(tileset->IsTileSolid(4));
    }

    // -------------------------------------------------------------------------
    // Grid access
    // -------------------------------------------------------------------------

    TEST(TilemapGrid, StoresTileIndicesBiasedByOneSoZeroMeansEmpty)
    {
        TilemapComponent map;
        map.Width = 4;
        map.Height = 4;
        const sizet layer = map.AddLayer("Ground");

        EXPECT_EQ(map.GetTile(layer, 2, 2), TilemapComponent::kEmptyTile);
        EXPECT_TRUE(map.SetTile(layer, 2, 2, 1)); // tileset tile 0
        EXPECT_EQ(map.GetTile(layer, 2, 2), 1u);
        EXPECT_TRUE(map.SetTile(layer, 2, 2, TilemapComponent::kEmptyTile));
        EXPECT_EQ(map.GetTile(layer, 2, 2), TilemapComponent::kEmptyTile);
    }

    TEST(TilemapGrid, RefusesOutOfRangeCoordinatesAndLayers)
    {
        TilemapComponent map;
        map.Width = 4;
        map.Height = 4;
        const sizet layer = map.AddLayer("Ground");

        EXPECT_FALSE(map.SetTile(layer, 4, 0, 1));
        EXPECT_FALSE(map.SetTile(layer, 0, 4, 1));
        EXPECT_FALSE(map.SetTile(layer + 1, 0, 0, 1));
        EXPECT_EQ(map.GetTile(layer, 99, 99), TilemapComponent::kEmptyTile);
        EXPECT_EQ(map.GetTile(layer + 1, 0, 0), TilemapComponent::kEmptyTile);
    }

    TEST(TilemapGrid, AShortLayerReadsAsEmptyRatherThanOutOfBounds)
    {
        // The shape a truncated or hand-edited scene file produces: Width/Height
        // say 4x4 but the vector only holds part of it.
        TilemapComponent map;
        map.Width = 4;
        map.Height = 4;
        TileLayer layer;
        layer.Tiles = { 1, 2, 3 };
        map.Layers.push_back(layer);

        EXPECT_EQ(map.GetTile(0, 0, 0), 1u);
        EXPECT_EQ(map.GetTile(0, 2, 0), 3u);
        EXPECT_EQ(map.GetTile(0, 3, 0), TilemapComponent::kEmptyTile);
        EXPECT_EQ(map.GetTile(0, 3, 3), TilemapComponent::kEmptyTile);
    }

    TEST(TilemapGrid, WritingIntoAShortLayerGrowsItToTheFullGrid)
    {
        TilemapComponent map;
        map.Width = 4;
        map.Height = 4;
        TileLayer layer;
        layer.Tiles = { 1 };
        map.Layers.push_back(layer);

        ASSERT_TRUE(map.SetTile(0, 3, 3, 9));
        EXPECT_EQ(map.Layers[0].Tiles.size(), 16u);
        EXPECT_EQ(map.GetTile(0, 3, 3), 9u);
        EXPECT_EQ(map.GetTile(0, 0, 0), 1u); // the pre-existing entry survives
    }

    TEST(TilemapGrid, ResizeKeepsTheTilesStillInsideTheGrid)
    {
        TilemapComponent map;
        map.Width = 4;
        map.Height = 4;
        const sizet layer = map.AddLayer("Ground");
        ASSERT_TRUE(map.SetTile(layer, 0, 0, 11));
        ASSERT_TRUE(map.SetTile(layer, 3, 3, 22));

        map.Resize(2, 2);
        EXPECT_EQ(map.Width, 2u);
        EXPECT_EQ(map.Height, 2u);
        EXPECT_EQ(map.GetTile(layer, 0, 0), 11u); // inside the new grid
        EXPECT_EQ(map.Layers[layer].Tiles.size(), 4u);

        // Growing back leaves the dropped cell empty rather than resurrecting it
        // from stale storage.
        map.Resize(4, 4);
        EXPECT_EQ(map.GetTile(layer, 0, 0), 11u);
        EXPECT_EQ(map.GetTile(layer, 3, 3), TilemapComponent::kEmptyTile);
    }

    TEST(TilemapGrid, ResizeRefusesADegenerateGrid)
    {
        TilemapComponent map;
        map.Width = 4;
        map.Height = 4;
        map.Resize(0, 8);
        EXPECT_EQ(map.Width, 4u);
        EXPECT_EQ(map.Height, 4u);
    }

    // -------------------------------------------------------------------------
    // Collider merging
    // -------------------------------------------------------------------------

    TEST(TilemapColliderMerge, EmitsNothingForAnEmptyMask)
    {
        const std::vector<bool> mask(16, false);
        EXPECT_TRUE(TilemapCollider::MergeSolidRuns(mask, 4, 4).empty());
    }

    TEST(TilemapColliderMerge, MergesAFullGridIntoASingleRectangle)
    {
        const std::vector<bool> mask(16, true);
        const auto rects = TilemapCollider::MergeSolidRuns(mask, 4, 4);
        ASSERT_EQ(rects.size(), 1u);
        EXPECT_EQ(rects[0], (TileColliderRect{ 0, 0, 4, 4 }));
    }

    TEST(TilemapColliderMerge, MergesAHorizontalRunIntoOneWideRectangle)
    {
        // Row 0 solid, everything else empty — a floor.
        std::vector<bool> mask(16, false);
        for (u32 x = 0; x < 4; ++x)
            mask[x] = true;

        const auto rects = TilemapCollider::MergeSolidRuns(mask, 4, 4);
        ASSERT_EQ(rects.size(), 1u);
        EXPECT_EQ(rects[0], (TileColliderRect{ 0, 0, 4, 1 }));
    }

    TEST(TilemapColliderMerge, MergesAVerticalRunIntoOneTallRectangle)
    {
        // Column 0 solid — a wall. This is the case a row-only merger gets wrong:
        // it would emit four 1x1 boxes.
        std::vector<bool> mask(16, false);
        for (u32 y = 0; y < 4; ++y)
            mask[y * 4] = true;

        const auto rects = TilemapCollider::MergeSolidRuns(mask, 4, 4);
        ASSERT_EQ(rects.size(), 1u);
        EXPECT_EQ(rects[0], (TileColliderRect{ 0, 0, 1, 4 }));
    }

    TEST(TilemapColliderMerge, CoversEverySolidCellExactlyOnce)
    {
        // A deliberately awkward shape (an L plus a detached block) checked by
        // reconstruction: every solid cell is covered, no empty cell is, and no
        // cell is covered twice. That is the property, independent of how the
        // greedy scan happens to split it.
        constexpr u32 kW = 6;
        constexpr u32 kH = 5;
        std::vector<bool> mask(kW * kH, false);
        auto set = [&](u32 x, u32 y)
        { mask[y * kW + x] = true; };
        for (u32 x = 0; x < 4; ++x)
            set(x, 0);
        for (u32 y = 0; y < 4; ++y)
            set(0, y);
        set(4, 3);
        set(5, 3);
        set(4, 4);

        const auto rects = TilemapCollider::MergeSolidRuns(mask, kW, kH);
        ASSERT_FALSE(rects.empty());

        std::vector<int> coverage(kW * kH, 0);
        for (const auto& r : rects)
        {
            for (u32 y = r.Y; y < r.Y + r.Height; ++y)
            {
                for (u32 x = r.X; x < r.X + r.Width; ++x)
                {
                    ASSERT_LT(y * kW + x, coverage.size());
                    ++coverage[y * kW + x];
                }
            }
        }

        for (u32 i = 0; i < kW * kH; ++i)
            EXPECT_EQ(coverage[i], mask[i] ? 1 : 0) << "cell " << i;
    }

    TEST(TilemapColliderMerge, ProducesFarFewerShapesThanSolidTiles)
    {
        // The whole point of merging: a 32x32 solid floor must not become 1024
        // Box2D polygons. Pinned as a hard bound, not a ratio, so a regression to
        // per-tile boxes fails loudly.
        constexpr u32 kW = 32;
        constexpr u32 kH = 32;
        const std::vector<bool> mask(kW * kH, true);
        const auto rects = TilemapCollider::MergeSolidRuns(mask, kW, kH);
        EXPECT_EQ(rects.size(), 1u);

        // A checkerboard is the worst case — nothing can merge — and is the upper
        // bound the greedy scan must still not exceed.
        std::vector<bool> checker(kW * kH, false);
        u32 solidCount = 0;
        for (u32 y = 0; y < kH; ++y)
        {
            for (u32 x = 0; x < kW; ++x)
            {
                if (((x + y) % 2) == 0)
                {
                    checker[y * kW + x] = true;
                    ++solidCount;
                }
            }
        }
        EXPECT_EQ(TilemapCollider::MergeSolidRuns(checker, kW, kH).size(), solidCount);
    }

    TEST(TilemapColliderMerge, TreatsAShortMaskAsNonSolidPastItsEnd)
    {
        // Same forgiving rule as GetTile: a truncated mask loses collision for the
        // missing cells instead of reading out of bounds.
        const std::vector<bool> mask(4, true);
        const auto rects = TilemapCollider::MergeSolidRuns(mask, 4, 4);
        ASSERT_EQ(rects.size(), 1u);
        EXPECT_EQ(rects[0], (TileColliderRect{ 0, 0, 4, 1 }));
    }

    TEST(TilemapColliderMask, OnlySolidLayersWithSolidTilesetEntriesContribute)
    {
        auto tileset = MakeTileset();
        TileInfo solid;
        solid.Solid = true;
        tileset->SetTileInfo(0, solid); // biased entry 1
        // Tile index 1 (biased entry 2) is left non-solid.

        TilemapComponent map;
        map.Width = 3;
        map.Height = 1;

        const sizet solidLayer = map.AddLayer("Ground");
        map.Layers[solidLayer].Solid = true;
        ASSERT_TRUE(map.SetTile(solidLayer, 0, 0, 1)); // solid tile
        ASSERT_TRUE(map.SetTile(solidLayer, 1, 0, 2)); // non-solid tile

        const sizet decorLayer = map.AddLayer("Decor");
        map.Layers[decorLayer].Solid = false;
        ASSERT_TRUE(map.SetTile(decorLayer, 2, 0, 1)); // solid tile on a non-solid layer

        const auto mask = TilemapCollider::BuildSolidMask(map, tileset);
        ASSERT_EQ(mask.size(), 3u);
        EXPECT_TRUE(mask[0]);
        EXPECT_FALSE(mask[1]);
        EXPECT_FALSE(mask[2]);
    }

    TEST(TilemapColliderMask, ProducesNoCollisionWithoutATileset)
    {
        TilemapComponent map;
        map.Width = 2;
        map.Height = 2;
        const sizet layer = map.AddLayer("Ground");
        map.Layers[layer].Solid = true;
        ASSERT_TRUE(map.SetTile(layer, 0, 0, 1));

        // Without per-tile metadata there is nothing that says "solid". Inventing
        // collision here would be worse than none: the author would get a wall
        // where the tileset says there is a decoration.
        const auto mask = TilemapCollider::BuildSolidMask(map, nullptr);
        EXPECT_EQ(std::count(mask.begin(), mask.end(), true), 0);
    }
} // namespace OloEngine::Tests
