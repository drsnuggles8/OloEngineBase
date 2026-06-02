#include "OloEnginePCH.h"

// =============================================================================
// WaterBuoyancyTest — Functional / cross-subsystem test for WATER §5.1.
//
// Seam under test: the renderer's Gerstner wave field (Renderer/WaterSurface,
// the CPU mirror of Water.glsl) driving Physics3D (Jolt) via BuoyancySystem,
// all inside a real Scene::OnUpdateRuntime tick. We drop dynamic boxes onto a
// WaterComponent surface and assert the emergent physical behaviour:
//   * a body lighter than its displaced water settles at the waterline,
//   * it tracks the water plane height (raise the plane → it floats higher),
//   * a body denser than water sinks straight through,
//   * with no water present it free-falls (the system is correctly gated),
//   * on a frozen wavy surface it rests at the height the CPU sampler reports
//     (the actual wave-field ↔ physics coupling).
//
// Functional-test contract: ADR 0001/0002/0003, docs/testing.md §7. Time is
// frozen for the wavy case so the surface — and therefore the rest height — is
// deterministic.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/WaterSurface.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // A body whose mass is half the water it displaces (density 1000, 1 m^3 box)
    // floats with its centre at the waterline. Tuned so the corner-probe model's
    // equilibrium sits at the plane: 250 kg ⇒ centre at y = planeY.
    constexpr f32 kBoxHalfExtent = 0.5f;
    constexpr f32 kFloatingMass = 250.0f;
    constexpr f32 kSubmergenceRamp = 1.0f; // full box height → continuous restoring force
} // namespace

class WaterBuoyancyTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Scenes are assembled per-test in the body so each scenario is explicit.
    }

    void TearDown() override
    {
        // Always release any frozen wave clock, even if an ASSERT aborted the
        // body, so a leaked mock time can't desync later tests in the process.
        Time::ClearMockTime();
        FunctionalTest::TearDown();
    }

    Entity SpawnWater(f32 planeY, f32 amplitude)
    {
        Entity water = GetScene().CreateEntity("Water");
        water.GetComponent<TransformComponent>().Translation = { 0.0f, planeY, 0.0f };
        auto& wc = water.AddComponent<WaterComponent>();
        wc.m_Enabled = true;
        wc.m_WorldSizeX = 200.0f;
        wc.m_WorldSizeZ = 200.0f;
        wc.m_WaveAmplitude = amplitude;
        return water;
    }

    Entity SpawnBuoyantBox(const glm::vec3& pos, f32 mass)
    {
        Entity box = GetScene().CreateEntity("Buoy");
        box.GetComponent<TransformComponent>().Translation = pos;

        auto& body = box.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = mass;
        body.m_LinearDrag = 0.0f; // buoyancy supplies its own submerged drag
        body.m_AngularDrag = 0.0f;

        auto& col = box.AddComponent<BoxCollider3DComponent>();
        col.m_HalfExtents = glm::vec3(kBoxHalfExtent);

        auto& buoyancy = box.AddComponent<BuoyancyComponent>();
        buoyancy.m_ProbeExtents = glm::vec3(kBoxHalfExtent);
        buoyancy.m_FluidDensity = 1000.0f;
        buoyancy.m_SubmergenceRamp = kSubmergenceRamp;
        buoyancy.m_LinearDrag = 4.0f; // settle quickly within the test window
        buoyancy.m_AngularDrag = 2.0f;
        return box;
    }

    static f32 Y(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation.y;
    }
};

TEST_F(WaterBuoyancyTest, LightBodySettlesAtWaterline)
{
    SpawnWater(/*planeY=*/0.0f, /*amplitude=*/0.0f); // flat water
    Entity box = SpawnBuoyantBox({ 0.0f, 5.0f, 0.0f }, kFloatingMass);
    EnablePhysics3D();

    ASSERT_GT(Y(box), 4.0f) << "box should start above the water";

    TickFor(/*totalSeconds=*/15.0f);

    // Floats: centre rests at the waterline (y≈0), neither sunk nor ejected.
    EXPECT_NEAR(Y(box), 0.0f, 0.35f) << "box did not settle at the waterline; y=" << Y(box);
    EXPECT_TRUE(std::isfinite(Y(box)));
}

TEST_F(WaterBuoyancyTest, FloatHeightTracksTheWaterPlane)
{
    // Same body, but the water plane is raised by 3 m — it must float 3 m higher.
    SpawnWater(/*planeY=*/3.0f, /*amplitude=*/0.0f);
    Entity box = SpawnBuoyantBox({ 0.0f, 9.0f, 0.0f }, kFloatingMass);
    EnablePhysics3D();

    TickFor(15.0f);

    EXPECT_NEAR(Y(box), 3.0f, 0.35f) << "buoyant body did not track the raised water plane; y=" << Y(box);
}

TEST_F(WaterBuoyancyTest, DenseBodySinks)
{
    // 5000 kg in a 1 m^3 box ⇒ density 5× water. Buoyancy can't hold it: it sinks
    // straight through and keeps going (there is no floor).
    SpawnWater(/*planeY=*/0.0f, /*amplitude=*/0.0f);
    Entity box = SpawnBuoyantBox({ 0.0f, 5.0f, 0.0f }, /*mass=*/5000.0f);
    EnablePhysics3D();

    TickFor(6.0f);

    EXPECT_LT(Y(box), -1.0f) << "dense body should have sunk well below the surface; y=" << Y(box);
}

TEST_F(WaterBuoyancyTest, WithoutWaterTheBodyFreeFalls)
{
    // BuoyancyComponent but no water in the scene ⇒ system is a no-op; gravity wins.
    Entity box = SpawnBuoyantBox({ 0.0f, 5.0f, 0.0f }, kFloatingMass);
    EnablePhysics3D();

    TickFor(3.0f);

    EXPECT_LT(Y(box), 0.0f) << "with no water the body must free-fall, not float; y=" << Y(box);
}

TEST_F(WaterBuoyancyTest, RestsAtTheWaveSurfaceHeight)
{
    // Freeze time so the (wavy) surface is static, then assert the body settles at
    // the height the CPU sampler reports for its column — the real wave-field ↔
    // physics coupling. Offset in X so the primary waves (zero-phase at the origin)
    // actually displace the surface there.
    Time::SetMockTime(0.0f);

    Entity water = SpawnWater(/*planeY=*/0.0f, /*amplitude=*/0.5f); // default-ish waves
    const glm::vec3 spawnPos{ 3.0f, 5.0f, 0.0f };
    Entity box = SpawnBuoyantBox(spawnPos, kFloatingMass);
    EnablePhysics3D();

    // Expected rest height: the sampler's surface height over the body's column,
    // built from the same WaterComponent the system reads.
    const auto& wc = water.GetComponent<WaterComponent>();
    WaterSurface::Params params;
    params.m_WaveDir0 = wc.PackWaveDir0();
    params.m_WaveDir1 = wc.PackWaveDir1();
    params.m_WaveFrequency = wc.m_WaveFrequency;
    params.m_WaveAmplitude = wc.m_WaveAmplitude;
    params.m_WaveSpeed = wc.m_WaveSpeed;
    params.m_PlaneHeight = 0.0f;
    const f32 expectedSurfaceY = WaterSurface::SampleHeight(params, { spawnPos.x, spawnPos.z }, 0.0f);

    // Sanity: the frozen surface really is displaced here (not a flat plane), so
    // this test genuinely exercises the wave field.
    ASSERT_GT(std::abs(expectedSurfaceY), 0.05f) << "chosen sample point is ~flat; pick another";

    TickFor(15.0f);

    EXPECT_NEAR(Y(box), expectedSurfaceY, 0.35f)
        << "floating body did not rest at the sampled wave height; y=" << Y(box)
        << " expected≈" << expectedSurfaceY;

    Time::ClearMockTime();
}
