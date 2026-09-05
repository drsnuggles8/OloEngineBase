#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// TilemapCollidersStopABodyTest — Functional Test.
//
// Cross-subsystem seam under test:
//   TilemapComponent x Tileset metadata x Scene::BuildTilemapColliders x Box2D,
//   driven through real Scene::OnUpdateRuntime ticks.
//
// TilemapTest.cpp pins the greedy rectangle merge as pure arithmetic, but
// nothing between that and a running physics world was covered: the merged
// rects still have to become real b2 shapes, at the right place, in the right
// size, on the right body. Every step there is a chance to be off by half a
// tile or to build nothing at all, and none of it shows up in the renderer.
//
// So this drops a dynamic Rigidbody2D onto a tilemap floor and asserts it stops
// on top of the surface rather than falling through it. The negative case —
// the identical scene with GenerateColliders off — must fall, which is what
// proves the body is being stopped BY THE TILEMAP and not by some other part of
// the 2D world (or by a stuck simulation that never moves anything at all).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Tilemap/Tileset.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr u32 kMapWidth = 16;
    constexpr u32 kMapHeight = 8;
    constexpr f32 kTileSize = 1.0f;

    // The floor occupies tile rows 0..2, so its top surface is at local y = 3.
    constexpr u32 kFloorRows = 3;
    constexpr f32 kFloorTopY = static_cast<f32>(kFloorRows) * kTileSize;

    constexpr f32 kBodyHalfExtent = 0.25f;
    constexpr f32 kDropHeight = 6.0f;
} // namespace

class TilemapCollidersStopABodyTest : public FunctionalTest
{
  protected:
    // A tileset whose tile 0 is solid; that is all the collider builder reads.
    static AssetHandle MakeSolidTileset()
    {
        auto tileset = Ref<Tileset>::Create();
        tileset->SetTileSize(16, 16);
        TileInfo solid;
        solid.Solid = true;
        tileset->SetTileInfo(0, solid);
        return AssetManager::AddMemoryOnlyAsset<Tileset>(tileset);
    }

    // A flat floor of solid tiles, `generateColliders` deciding whether it
    // becomes physics at all.
    Entity SpawnTilemap(AssetHandle tilesetHandle, bool generateColliders)
    {
        Entity entity = GetScene().CreateEntity("Floor");
        entity.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };

        auto& map = entity.AddComponent<TilemapComponent>();
        map.TilesetHandle = tilesetHandle;
        map.Width = kMapWidth;
        map.Height = kMapHeight;
        map.TileSize = kTileSize;
        map.GenerateColliders = generateColliders;

        const sizet ground = map.AddLayer("Ground");
        map.Layers[ground].Solid = true;
        for (u32 y = 0; y < kFloorRows; ++y)
        {
            for (u32 x = 0; x < kMapWidth; ++x)
                map.SetTile(ground, x, y, 1); // biased: tileset tile 0
        }
        return entity;
    }

    Entity SpawnFallingBody(f32 x, f32 y)
    {
        Entity entity = GetScene().CreateEntity("Falling");
        entity.GetComponent<TransformComponent>().Translation = { x, y, 0.0f };

        auto& rb = entity.AddComponent<Rigidbody2DComponent>();
        rb.Type = Rigidbody2DComponent::BodyType::Dynamic;
        rb.FixedRotation = true;

        auto& box = entity.AddComponent<BoxCollider2DComponent>();
        box.Size = { kBodyHalfExtent, kBodyHalfExtent };
        box.Density = 1.0f;
        box.Friction = 0.3f;
        return entity;
    }

    void BuildScene() override
    {
        // Only the asset manager here: AddMemoryOnlyAsset needs one, and each
        // test builds its own entities before starting physics.
        EnableAssetManager({});
    }

    // EnablePhysics2D runs OnPhysics2DStart, which is what creates the Box2D
    // bodies — including the tilemap's. It therefore has to be called AFTER the
    // entities exist, which is why every test ends its setup with this.
    void StartPhysicsAndSettle()
    {
        EnablePhysics2D();
        RunFrames(180); // ~3 s: long enough to fall 6 units and come to rest
    }
};

TEST_F(TilemapCollidersStopABodyTest, ASolidTilemapFloorStopsAFallingBody)
{
    const AssetHandle tileset = MakeSolidTileset();
    ASSERT_NE(static_cast<u64>(tileset), 0ULL);

    SpawnTilemap(tileset, /*generateColliders=*/true);
    Entity falling = SpawnFallingBody(kMapWidth * 0.5f, kDropHeight);
    StartPhysicsAndSettle();

    const f32 restY = falling.GetComponent<TransformComponent>().Translation.y;

    // It must have fallen (the sim actually ran)...
    EXPECT_LT(restY, kDropHeight - 0.5f)
        << "the body never moved — the 2D simulation is not stepping";

    // ...and come to rest on the floor's top surface, within a tolerance that
    // absorbs Box2D's contact slop rather than demanding an exact contact point.
    EXPECT_NEAR(restY, kFloorTopY + kBodyHalfExtent, 0.25f)
        << "expected the body to rest on the tilemap surface at y=" << kFloorTopY
        << "; it is at y=" << restY
        << (restY < kFloorTopY ? " — it fell THROUGH the generated collision" : "");
}

TEST_F(TilemapCollidersStopABodyTest, TheSameMapWithoutColliderGenerationDoesNotStopIt)
{
    // The control. Without it, a body that simply never moved would pass the
    // test above, and so would one stopped by something other than the tilemap.
    const AssetHandle tileset = MakeSolidTileset();
    ASSERT_NE(static_cast<u64>(tileset), 0ULL);

    SpawnTilemap(tileset, /*generateColliders=*/false);
    Entity falling = SpawnFallingBody(kMapWidth * 0.5f, kDropHeight);
    StartPhysicsAndSettle();

    const f32 restY = falling.GetComponent<TransformComponent>().Translation.y;
    EXPECT_LT(restY, kFloorTopY - 1.0f)
        << "the body stopped at y=" << restY
        << " with GenerateColliders off — something other than the tilemap is holding it up, "
           "which would make the positive case prove nothing";
}

TEST_F(TilemapCollidersStopABodyTest, ATilemapWithNoSolidTilesetEntriesGeneratesNoCollision)
{
    // The tileset is what says "solid". A tilemap whose tiles are all non-solid
    // must not gain collision just because the layer is marked Solid — otherwise
    // decoration layers would silently become walls.
    auto tileset = Ref<Tileset>::Create();
    tileset->SetTileSize(16, 16); // every tile left at its non-solid default
    const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<Tileset>(tileset);
    ASSERT_NE(static_cast<u64>(handle), 0ULL);

    SpawnTilemap(handle, /*generateColliders=*/true);
    Entity falling = SpawnFallingBody(kMapWidth * 0.5f, kDropHeight);
    StartPhysicsAndSettle();

    EXPECT_LT(falling.GetComponent<TransformComponent>().Translation.y, kFloorTopY - 1.0f)
        << "non-solid tileset entries still produced collision";
}
