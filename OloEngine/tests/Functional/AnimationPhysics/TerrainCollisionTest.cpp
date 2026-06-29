#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// TerrainCollisionTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Terrain (TerrainComponent / TerrainData CPU height field) × Physics3D
//   (Jolt HeightFieldShape static body + dynamic rigidbodies + scene raycasts)
//   driven by the real Scene::OnPhysics3DStart / OnUpdateRuntime path.
//
//   Before issue #428 terrain had NO collision: bodies fell straight through it,
//   raycasts missed it. These tests pin the new wiring — a TerrainComponent with
//   m_CollisionEnabled produces a static Jolt height-field body that:
//     (a) holds a dropped dynamic body up at the terrain surface (flat & sloped),
//     (b) follows the terrain shape (rest height tracks the local surface), and
//     (c) is hit by scene raycasts, resolving back to the terrain entity.
//
// Headless: the collision body is built from CPU heights generated GPU-free
// (TerrainGenerator::GenerateHeightField — the same field the render path's
// GenerateHeightmap uploads), so no GL context is needed. See docs/testing.md §7.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Procedural terrain parameters shared by the component setup and the
    // expected-surface reconstruction below — keep the two in lock-step.
    // Resolution is deliberately ODD (65) to exercise the height-field's
    // even-padding path; worldSize / (resolution-1) = 1.0 keeps the grid->world
    // mapping exact and easy to reason about.
    constexpr u32 kProcResolution = 65;
    constexpr f32 kProcWorldSize = 64.0f;
    constexpr f32 kProcHeightScale = 4.0f;
    constexpr i32 kProcSeed = 1337;
    constexpr u32 kProcOctaves = 4;
    constexpr f32 kProcFrequency = 3.0f;
    constexpr f32 kProcLacunarity = 2.0f;
    constexpr f32 kProcPersistence = 0.5f;

    TerrainGenerator::HeightParams MakeProcParams()
    {
        TerrainGenerator::HeightParams params;
        params.Resolution = kProcResolution;
        params.Seed = kProcSeed;
        params.Octaves = kProcOctaves;
        params.Frequency = kProcFrequency;
        params.Lacunarity = kProcLacunarity;
        params.Persistence = kProcPersistence;
        return params;
    }

    // Bilinear sample of a normalized [0,1] row-major height field — mirrors
    // TerrainData::GetHeightAt so the test's expectation matches the field the
    // collision shape was built from.
    f32 SampleHeight01(const std::vector<f32>& heights, u32 resolution, f32 nx, f32 nz)
    {
        nx = std::clamp(nx, 0.0f, 1.0f);
        nz = std::clamp(nz, 0.0f, 1.0f);
        const f32 fx = nx * static_cast<f32>(resolution - 1);
        const f32 fz = nz * static_cast<f32>(resolution - 1);
        const u32 x0 = static_cast<u32>(fx);
        const u32 z0 = static_cast<u32>(fz);
        const u32 x1 = std::min(x0 + 1, resolution - 1);
        const u32 z1 = std::min(z0 + 1, resolution - 1);
        const f32 tx = fx - static_cast<f32>(x0);
        const f32 tz = fz - static_cast<f32>(z0);
        const f32 h00 = heights[static_cast<sizet>(z0) * resolution + x0];
        const f32 h10 = heights[static_cast<sizet>(z0) * resolution + x1];
        const f32 h01 = heights[static_cast<sizet>(z1) * resolution + x0];
        const f32 h11 = heights[static_cast<sizet>(z1) * resolution + x1];
        const f32 a = h00 + tx * (h10 - h00);
        const f32 b = h01 + tx * (h11 - h01);
        return a + tz * (b - a);
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Flat terrain, translated up via its TransformComponent. Proves the height-field
// body exists, holds a body up (no fall-through), AND that the entity transform
// translation is applied to the collision surface.
// ─────────────────────────────────────────────────────────────────────────────
class TerrainCollisionFlatTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Flat terrain (no procedural, no heightmap) sitting at world Y = 10.
        m_Terrain = GetScene().CreateEntity("Terrain");
        m_Terrain.GetComponent<TransformComponent>().Translation = { 0.0f, kTerrainY, 0.0f };
        auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
        terrain.m_ProceduralEnabled = false;
        terrain.m_HeightmapPath.clear();
        terrain.m_CollisionEnabled = true;
        // Flat collision spans local [0, WorldSize] in X/Z (256 default), so the
        // ball's XZ must land inside that footprint.

        // Dynamic sphere dropped from above the surface.
        m_Ball = GetScene().CreateEntity("Ball");
        m_Ball.GetComponent<TransformComponent>().Translation = { 128.0f, kTerrainY + 6.0f, 128.0f };
        auto& body = m_Ball.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_LinearDrag = 0.0f;
        auto& col = m_Ball.AddComponent<SphereCollider3DComponent>();
        col.m_Radius = kRadius;

        EnablePhysics3D();
    }

    static constexpr f32 kTerrainY = 10.0f;
    static constexpr f32 kRadius = 0.5f;

    Entity m_Terrain;
    Entity m_Ball;
};

TEST_F(TerrainCollisionFlatTest, DynamicBodyRestsOnFlatTerrainSurface)
{
    const auto BallY = [this]
    { return m_Ball.GetComponent<TransformComponent>().Translation.y; };

    ASSERT_GT(BallY(), kTerrainY + 2.0f) << "ball should start well above the terrain";

    // Settle on the static surface (5 s of fixed-step sim is ample).
    TickFor(5.0f);

    const auto& pos = m_Ball.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z))
        << "ball transform contains NaN/Inf";
    // Did NOT fall through: a missing collider would have let it drop far below 10.
    EXPECT_GT(pos.y, kTerrainY - 0.5f) << "ball fell through the terrain; y=" << pos.y;
    // Rests exactly one radius above the flat surface (constant field ⇒ no slope /
    // quantization error to budget for).
    EXPECT_NEAR(pos.y, kTerrainY + kRadius, 0.1f)
        << "ball did not settle on the flat terrain surface; y=" << pos.y;
}

// ─────────────────────────────────────────────────────────────────────────────
// Procedural terrain. Proves the collision surface FOLLOWS the height field
// (rest height tracks the local terrain surface) and that raycasts hit it.
// ─────────────────────────────────────────────────────────────────────────────
class TerrainCollisionProceduralTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Terrain = GetScene().CreateEntity("Terrain");
        // Terrain at the world origin, identity scale/rotation: local == world.
        auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
        terrain.m_ProceduralEnabled = true;
        terrain.m_HeightmapPath.clear();
        terrain.m_CollisionEnabled = true;
        terrain.m_WorldSizeX = kProcWorldSize;
        terrain.m_WorldSizeZ = kProcWorldSize;
        terrain.m_HeightScale = kProcHeightScale;
        terrain.m_ProceduralResolution = kProcResolution;
        terrain.m_ProceduralSeed = kProcSeed;
        terrain.m_ProceduralOctaves = kProcOctaves;
        terrain.m_ProceduralFrequency = kProcFrequency;
        terrain.m_ProceduralLacunarity = kProcLacunarity;
        terrain.m_ProceduralPersistence = kProcPersistence;

        // Dynamic sphere dropped over the terrain centre (well above the [0,scale]
        // height band).
        m_Ball = GetScene().CreateEntity("Ball");
        m_Ball.GetComponent<TransformComponent>().Translation = { kBallX, kProcHeightScale + 8.0f, kBallZ };
        auto& body = m_Ball.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_LinearDrag = 0.0f;
        auto& col = m_Ball.AddComponent<SphereCollider3DComponent>();
        col.m_Radius = kRadius;

        // Reconstruct the exact CPU field the collision body was built from.
        TerrainGenerator::GenerateHeightField(m_Heights, MakeProcParams());

        EnablePhysics3D();
    }

    // World-space terrain surface Y at a world XZ (terrain at origin, identity xform).
    f32 ExpectedSurfaceY(f32 worldX, f32 worldZ) const
    {
        const f32 nx = worldX / kProcWorldSize;
        const f32 nz = worldZ / kProcWorldSize;
        return SampleHeight01(m_Heights, kProcResolution, nx, nz) * kProcHeightScale;
    }

    static constexpr f32 kRadius = 0.5f;
    static constexpr f32 kBallX = 32.0f;
    static constexpr f32 kBallZ = 32.0f;

    Entity m_Terrain;
    Entity m_Ball;
    std::vector<f32> m_Heights;
};

TEST_F(TerrainCollisionProceduralTest, DynamicBodyRestsOnProceduralSurface)
{
    ASSERT_FALSE(m_Heights.empty()) << "height field reconstruction failed";

    const auto BallPos = [this]
    { return m_Ball.GetComponent<TransformComponent>().Translation; };
    ASSERT_GT(BallPos().y, kProcHeightScale + 2.0f) << "ball should start above the terrain band";

    TickFor(5.0f);

    const auto pos = BallPos();
    EXPECT_TRUE(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z))
        << "ball transform contains NaN/Inf";

    // Sample the surface at the ball's FINAL XZ (it may have rolled slightly on a
    // slope) and assert the ball rests one radius above it. Tolerance budgets for
    // sphere-on-slope contact offset, Jolt height quantization, and bilinear-vs-
    // triangle interpolation across a cell — all small on this gentle field.
    const f32 surfaceY = ExpectedSurfaceY(pos.x, pos.z);
    EXPECT_GT(pos.y, surfaceY - 0.25f) << "ball penetrated the terrain; y=" << pos.y << " surface=" << surfaceY;
    EXPECT_NEAR(pos.y, surfaceY + kRadius, 0.75f)
        << "ball did not settle on the procedural surface; y=" << pos.y << " surface=" << surfaceY;
}

TEST_F(TerrainCollisionProceduralTest, RaycastHitsTerrainSurface)
{
    ASSERT_FALSE(m_Heights.empty()) << "height field reconstruction failed";

    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr) << "scene has no JoltScene after EnablePhysics3D";

    // Cast straight down at a corner away from the falling ball, so only the
    // terrain is in the ray's path.
    constexpr f32 kProbeX = 16.0f;
    constexpr f32 kProbeZ = 16.0f;
    RayCastInfo ray;
    ray.m_Origin = { kProbeX, kProcHeightScale + 50.0f, kProbeZ };
    ray.m_Direction = { 0.0f, -1.0f, 0.0f };
    ray.m_MaxDistance = 100.0f;

    SceneQueryHit hit;
    const bool didHit = joltScene->CastRay(ray, hit);

    ASSERT_TRUE(didHit && hit.HasHit()) << "ray straight down missed the terrain";
    EXPECT_EQ(hit.m_HitEntity, m_Terrain.GetUUID()) << "raycast resolved to the wrong entity";
    EXPECT_TRUE(std::isfinite(hit.m_Position.y));

    const f32 surfaceY = ExpectedSurfaceY(kProbeX, kProbeZ);
    EXPECT_NEAR(hit.m_Position.y, surfaceY, 0.5f)
        << "ray hit the terrain at the wrong height; hit=" << hit.m_Position.y << " surface=" << surfaceY;
}
