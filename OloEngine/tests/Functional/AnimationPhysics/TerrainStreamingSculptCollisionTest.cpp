#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// TerrainStreamingSculptCollisionTest — Functional Test (issue #469).
//
// Cross-subsystem seam under test:
//   Terrain (streamed per-tile TerrainData / single-tile CPU height field +
//   sculpt edits) × Physics3D (Jolt HeightFieldShape static bodies, per-tile
//   bodies, in-place SetHeights updates, dynamic rigidbodies, scene raycasts)
//   driven by the real Scene::OnPhysics3DStart / OnUpdateRuntime path.
//
//   #428 (PR #468) gave SINGLE-TILE terrain collision. Two gaps remained, both
//   fixed here and pinned by these tests:
//     (1) STREAMED terrain had no collision at all — bodies fell through streamed
//         worlds. Now each loaded tile gets a static height-field body keyed by
//         (terrain entity, tile grid X/Z); a body dropped on a tile FAR from the
//         origin rests on it, a raycast resolves to the terrain entity, and the
//         body is torn down when the tile is evicted.
//     (2) SCULPT / EROSION edits updated the mesh but not the collision shape —
//         a dropped body kept resting on the pre-edit surface. Now an edit over a
//         dirty rect refreshes the collision height field (JPH::HeightFieldShape::
//         SetHeights, block-aligned) so a raycast / dropped body reflects the NEW
//         surface.
//
// Headless: the full TerrainStreamer needs a GL context (its tile GPU build), so
// these tests drive the collision PRIMITIVES the render-path reconcile calls into
// (JoltScene::CreateTerrainTileBody / Scene::UpdateTerrainCollisionAfterEdit)
// directly, from CPU heights built GPU-free — no GL context required. The full
// streamer→collision + editor-sculpt integration is verified in the editor.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltShapes.h"
#include "OloEngine/Physics3D/SceneQueries.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Shared tile geometry. Odd resolution (65) exercises the height-field's even-
    // padding path; worldSize / (resolution-1) = 1.0 keeps grid->world exact.
    constexpr u32 kRes = 65;
    constexpr f32 kTileWorldSize = 64.0f;
    constexpr f32 kHeightScale = 4.0f;
    constexpr f32 kRadius = 0.5f;

    // A flat height field at a constant normalized height, whose world surface Y is
    // `normalized * kHeightScale`.
    std::vector<f32> FlatField(f32 normalized)
    {
        return std::vector<f32>(static_cast<sizet>(kRes) * kRes, normalized);
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// (1) STREAMED terrain — per-tile collision far from the origin.
//
// Builds one static tile body at grid (3,0) → world origin (192,0,0), the exact
// primitive the render-path streaming reconcile creates per loaded tile. Nothing
// at the world origin has collision, so a body resting here proves the streaming
// gap (bodies fell through tiles away from origin) is closed.
// ─────────────────────────────────────────────────────────────────────────────
class TerrainStreamingTileCollisionTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Terrain = GetScene().CreateEntity("StreamedTerrain");
        // Terrain at the world origin, identity transform: tile world origin == grid*size.
        auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
        terrain.m_StreamingEnabled = true;
        terrain.m_CollisionEnabled = true;
        terrain.m_HeightScale = kHeightScale;
        terrain.m_TileWorldSize = kTileWorldSize;
        terrain.m_TileResolution = kRes;

        EnablePhysics3D();

        // Create the per-tile body directly (the render path's reconcile is GL-coupled).
        // A tile raised to normalized 0.5 → world surface Y = 2.0.
        auto* jolt = GetScene().GetPhysicsScene();
        JPH::Ref<JPH::Shape> shape = JoltShapes::CreateTerrainHeightFieldShape(
            FlatField(0.5f), kRes, kTileWorldSize, kTileWorldSize, kHeightScale, glm::vec3(1.0f));
        if (shape)
        {
            jolt->CreateTerrainTileBody(m_Terrain, kTileGridX, kTileGridZ, shape,
                                        m_TileWorldOrigin, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        }
    }

    static constexpr i32 kTileGridX = 3;
    static constexpr i32 kTileGridZ = 0;
    // World origin of tile (3,0): (gridX*tileSize, 0, gridZ*tileSize).
    const glm::vec3 m_TileWorldOrigin{ static_cast<f32>(kTileGridX) * kTileWorldSize, 0.0f,
                                       static_cast<f32>(kTileGridZ) * kTileWorldSize };
    static constexpr f32 kTileSurfaceY = 0.5f * kHeightScale; // 2.0
    // Probe / drop over the centre of the far tile.
    const f32 kProbeX = m_TileWorldOrigin.x + kTileWorldSize * 0.5f; // 224
    const f32 kProbeZ = m_TileWorldOrigin.z + kTileWorldSize * 0.5f; // 32

    Entity m_Terrain;
};

TEST_F(TerrainStreamingTileCollisionTest, RaycastHitsStreamedTileFarFromOrigin)
{
    auto* jolt = GetScene().GetPhysicsScene();
    ASSERT_NE(jolt, nullptr);
    ASSERT_TRUE(jolt->HasTerrainTileBody(m_Terrain.GetUUID(), kTileGridX, kTileGridZ))
        << "tile body was not created";

    RayCastInfo ray;
    ray.m_Origin = { kProbeX, kTileSurfaceY + 50.0f, kProbeZ };
    ray.m_Direction = { 0.0f, -1.0f, 0.0f };
    ray.m_MaxDistance = 100.0f;

    SceneQueryHit hit;
    const bool didHit = jolt->CastRay(ray, hit);
    ASSERT_TRUE(didHit && hit.HasHit()) << "ray down over the far streamed tile missed";
    // UserData carries the OWNING TERRAIN ENTITY so queries resolve the terrain.
    EXPECT_EQ(hit.m_HitEntity, m_Terrain.GetUUID()) << "streamed tile resolved to the wrong entity";
    EXPECT_NEAR(hit.m_Position.y, kTileSurfaceY, 0.3f)
        << "streamed tile collision at the wrong height; hit=" << hit.m_Position.y;
}

TEST_F(TerrainStreamingTileCollisionTest, EvictingTileBodyRemovesCollision)
{
    auto* jolt = GetScene().GetPhysicsScene();
    ASSERT_NE(jolt, nullptr);

    RayCastInfo ray;
    ray.m_Origin = { kProbeX, kTileSurfaceY + 50.0f, kProbeZ };
    ray.m_Direction = { 0.0f, -1.0f, 0.0f };
    ray.m_MaxDistance = 100.0f;

    SceneQueryHit before;
    ASSERT_TRUE(jolt->CastRay(ray, before) && before.HasHit()) << "tile collision missing before evict";

    // Simulate the streamer evicting the tile (EvictOverBudget / out-of-range).
    jolt->DestroyTerrainTileBody(m_Terrain.GetUUID(), kTileGridX, kTileGridZ);
    EXPECT_FALSE(jolt->HasTerrainTileBody(m_Terrain.GetUUID(), kTileGridX, kTileGridZ));

    SceneQueryHit after;
    const bool stillHits = jolt->CastRay(ray, after);
    EXPECT_FALSE(stillHits && after.HasHit()) << "evicted tile collision leaked";
}

// Ball dropped onto a streamed tile far from the origin must rest on it.
class TerrainStreamingTileRestTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Terrain = GetScene().CreateEntity("StreamedTerrain");
        auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
        terrain.m_StreamingEnabled = true;
        terrain.m_CollisionEnabled = true;
        terrain.m_HeightScale = kHeightScale;
        terrain.m_TileWorldSize = kTileWorldSize;
        terrain.m_TileResolution = kRes;

        m_Ball = GetScene().CreateEntity("Ball");
        m_Ball.GetComponent<TransformComponent>().Translation = { kProbeX, kTileSurfaceY + 8.0f, kProbeZ };
        auto& body = m_Ball.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_LinearDrag = 0.0f;
        m_Ball.AddComponent<SphereCollider3DComponent>().m_Radius = kRadius;

        EnablePhysics3D();

        auto* jolt = GetScene().GetPhysicsScene();
        JPH::Ref<JPH::Shape> shape = JoltShapes::CreateTerrainHeightFieldShape(
            FlatField(0.5f), kRes, kTileWorldSize, kTileWorldSize, kHeightScale, glm::vec3(1.0f));
        if (shape)
        {
            jolt->CreateTerrainTileBody(m_Terrain, 3, 0, shape,
                                        glm::vec3(3.0f * kTileWorldSize, 0.0f, 0.0f),
                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        }
    }

    static constexpr f32 kTileSurfaceY = 0.5f * kHeightScale;                     // 2.0
    static constexpr f32 kProbeX = 3.0f * kTileWorldSize + kTileWorldSize * 0.5f; // 224
    static constexpr f32 kProbeZ = kTileWorldSize * 0.5f;                         // 32

    Entity m_Terrain;
    Entity m_Ball;
};

TEST_F(TerrainStreamingTileRestTest, DynamicBodyRestsOnStreamedTile)
{
    const auto BallY = [this]
    { return m_Ball.GetComponent<TransformComponent>().Translation.y; };
    ASSERT_GT(BallY(), kTileSurfaceY + 2.0f) << "ball should start above the tile";

    TickFor(5.0f);

    const auto& pos = m_Ball.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(pos.y)) << "ball transform contains NaN/Inf";
    EXPECT_GT(pos.y, kTileSurfaceY - 0.5f) << "ball fell through the streamed tile; y=" << pos.y;
    EXPECT_NEAR(pos.y, kTileSurfaceY + kRadius, 0.15f)
        << "ball did not settle on the streamed tile surface; y=" << pos.y;
}

// ─────────────────────────────────────────────────────────────────────────────
// (2) SCULPT edits — the collision shape must follow the edited height field.
//
// A single-tile FLAT terrain builds its collision body GPU-free at OnPhysics3DStart
// (BuildTerrainCollisionBody synthesizes a 256×256 zero field — no TerrainData / GL,
// same headless path as TerrainCollisionFlatTest). We then edit a matching CPU field
// and push the dirty rect through the SAME primitive the editor's sculpt/erosion
// path calls via Scene::UpdateTerrainCollisionAfterEdit — JoltScene::
// UpdateTerrainBodyHeights (block-aligned SetHeights). The full Scene wrapper +
// editor stroke wiring (which needs a GPU-backed TerrainData) is verified live in
// the editor.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    // Flat single-tile terrain params. BuildTerrainCollisionBody's flat path uses a fixed
    // 256×256 field spanning [0, WorldSize] in X/Z at HeightScale.
    constexpr u32 kSculptRes = 256;
    constexpr f32 kSculptWorldSize = 256.0f; // world per sample ≈ 256/255 ≈ 1.004
    constexpr f32 kSculptHeightScale = 4.0f;
    constexpr f32 kRidgeSurfaceY = 0.5f * kSculptHeightScale; // 2.0
    constexpr f32 kSculptCentre = 128.0f;                     // world XZ over the raised block

    TerrainComponent& MakeFlatSculptTerrain(Entity terrain)
    {
        auto& tc = terrain.AddComponent<TerrainComponent>();
        tc.m_ProceduralEnabled = false;
        tc.m_HeightmapPath.clear();
        tc.m_CollisionEnabled = true;
        tc.m_StreamingEnabled = false;
        tc.m_WorldSizeX = kSculptWorldSize;
        tc.m_WorldSizeZ = kSculptWorldSize;
        tc.m_HeightScale = kSculptHeightScale;
        return tc;
    }

    // Raise a central block of samples to normalized 0.5 (world Y = 2.0) in a flat 256²
    // field and return its dirty rect (x, z, w, h), block-aligned. World ≈ [96,160].
    struct Rect
    {
        u32 X, Z, W, H;
    };
    Rect RaiseCentralRidge(std::vector<f32>& heights)
    {
        constexpr u32 lo = 96;
        constexpr u32 hi = 160; // inclusive
        for (u32 z = lo; z <= hi; ++z)
            for (u32 x = lo; x <= hi; ++x)
                heights[static_cast<sizet>(z) * kSculptRes + x] = 0.5f;
        return Rect{ lo, lo, hi - lo + 1, hi - lo + 1 };
    }
} // namespace

class TerrainSculptCollisionTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Terrain = GetScene().CreateEntity("Terrain");
        MakeFlatSculptTerrain(m_Terrain);
        EnablePhysics3D(); // builds the flat 256² collision body GPU-free
        // Mirror the field the body was built from (all zero) so we can edit + re-push it.
        m_Heights.assign(static_cast<sizet>(kSculptRes) * kSculptRes, 0.0f);
    }

    Entity m_Terrain;
    std::vector<f32> m_Heights;
};

TEST_F(TerrainSculptCollisionTest, SculptedRidgeRaisesCollisionSurface)
{
    auto* jolt = GetScene().GetPhysicsScene();
    ASSERT_NE(jolt, nullptr);

    RayCastInfo ray;
    ray.m_Origin = { kSculptCentre, 50.0f, kSculptCentre };
    ray.m_Direction = { 0.0f, -1.0f, 0.0f };
    ray.m_MaxDistance = 100.0f;

    // Before the edit the surface is flat at Y = 0.
    SceneQueryHit before;
    ASSERT_TRUE(jolt->CastRay(ray, before) && before.HasHit()) << "flat terrain collision missing";
    EXPECT_NEAR(before.m_Position.y, 0.0f, 0.2f) << "flat surface not at Y=0; y=" << before.m_Position.y;

    // Sculpt a raised ridge into the CPU field and push the dirty rect to the collision
    // body via the same primitive the editor sculpt path calls.
    const Rect r = RaiseCentralRidge(m_Heights);
    ASSERT_TRUE(jolt->UpdateTerrainBodyHeights(m_Terrain.GetUUID(), r.X, r.Z, r.W, r.H, m_Heights, kSculptRes))
        << "collision height update reported no body";

    // The collision surface at the ridge centre now reflects the NEW height.
    SceneQueryHit after;
    ASSERT_TRUE(jolt->CastRay(ray, after) && after.HasHit()) << "collision missing after sculpt";
    EXPECT_EQ(after.m_HitEntity, m_Terrain.GetUUID());
    EXPECT_NEAR(after.m_Position.y, kRidgeSurfaceY, 0.3f)
        << "collision did not follow the sculpted ridge; y=" << after.m_Position.y;
}

// A body dropped onto the sculpted region rests on the NEW (raised) surface, not the
// pre-edit flat one.
class TerrainSculptRestTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Terrain = GetScene().CreateEntity("Terrain");
        MakeFlatSculptTerrain(m_Terrain);

        m_Ball = GetScene().CreateEntity("Ball");
        m_Ball.GetComponent<TransformComponent>().Translation = { kSculptCentre, kRidgeSurfaceY + 8.0f, kSculptCentre };
        auto& body = m_Ball.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_LinearDrag = 0.0f;
        m_Ball.AddComponent<SphereCollider3DComponent>().m_Radius = kRadius;

        EnablePhysics3D();
        m_Heights.assign(static_cast<sizet>(kSculptRes) * kSculptRes, 0.0f);
    }

    Entity m_Terrain;
    Entity m_Ball;
    std::vector<f32> m_Heights;
};

TEST_F(TerrainSculptRestTest, DynamicBodyRestsOnSculptedRidge)
{
    auto* jolt = GetScene().GetPhysicsScene();
    ASSERT_NE(jolt, nullptr);

    // Sculpt the ridge BEFORE any simulation advances, then sync collision.
    const Rect r = RaiseCentralRidge(m_Heights);
    ASSERT_TRUE(jolt->UpdateTerrainBodyHeights(m_Terrain.GetUUID(), r.X, r.Z, r.W, r.H, m_Heights, kSculptRes))
        << "collision height update reported no body";

    TickFor(5.0f);

    const auto& pos = m_Ball.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(pos.y)) << "ball transform contains NaN/Inf";
    // Must rest on the raised ridge (~2.5), NOT on the pre-edit flat surface (~0.5).
    EXPECT_NEAR(pos.y, kRidgeSurfaceY + kRadius, 0.2f)
        << "ball did not rest on the sculpted ridge; y=" << pos.y;
    EXPECT_GT(pos.y, 1.0f) << "ball rested on the stale pre-edit surface; y=" << pos.y;
}
