#include "OloEnginePCH.h"

// =============================================================================
// ParticleSystemEmitsAndExpiresTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × ParticleSystem update × particle pool lifecycle. Particle
//   systems on entities are stepped from inside `Scene::OnUpdateRuntime`
//   (the per-tick block at line ~1206 onward). A regression where the
//   per-entity update is dropped looks like "no particles ever appear in
//   gameplay" — and existing ParticleSystem unit tests don't catch it
//   because they call `Update()` directly, not via the scene path.
//
// Scenario: a non-looping ParticleSystemComponent emits a short burst then
// stops. We tick well past the longest particle lifetime and assert the
// pool drains to empty — proving both that emission ran *and* that the
// lifetime-decrement path keeps ticking after Playing flips to false.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class ParticleSystemEmitsAndExpiresTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Emitter = GetScene().CreateEntity("Emitter");
        m_Emitter.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };

        m_Emitter.AddComponent<ParticleSystemComponent>();
        auto& psc = m_Emitter.GetComponent<ParticleSystemComponent>();
        psc.System.Playing = true;
        psc.System.Looping = false;
        psc.System.Duration = 0.4f; // Stop emitting after 0.4s.
        psc.System.Emitter.RateOverTime = 50.0f;
        psc.System.Emitter.LifetimeMin = 0.3f;
        psc.System.Emitter.LifetimeMax = 0.5f;
        psc.System.Emitter.InitialSpeed = 0.0f;
        psc.System.Emitter.SpeedVariance = 0.0f;
    }

    [[nodiscard]] u32 AliveCount() const
    {
        return m_Emitter.GetComponent<ParticleSystemComponent>().System.GetAliveCount();
    }

    void SeedTrailsAndMoveSystem()
    {
        auto& system = m_Emitter.GetComponent<ParticleSystemComponent>().System;
        system.Playing = false;
        system.TrailModule.Enabled = true;
        system.TrailModule.TrailLifetime = 10.0f;
        auto& pool = system.GetPool();
        ASSERT_EQ(pool.Emit(3), 3u);
        for (u32 i = 0; i < 3; ++i)
        {
            pool.m_Positions[i] = { 10.0f * static_cast<f32>(i + 1), 1.0f, 0.0f };
            pool.m_Velocities[i] = glm::vec3(0.0f);
            pool.m_Lifetimes[i] = 5.0f;
            pool.m_MaxLifetimes[i] = 5.0f;
            pool.m_Sizes[i] = 1.0f;
            pool.m_Colors[i] = glm::vec4(1.0f);
        }
        RunFrames(1);
        ASSERT_GT(system.GetTrailData().GetTrail(2).m_Count, 0u);

        // Keep the non-trivial variant alternative active during relocation.
        // Its triangles/CDF must remain owned by the surviving system.
        EmitMesh mesh;
        const glm::vec3 positions[] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
        const u32 indices[] = { 0, 1, 2 };
        mesh.Build(positions, 3, indices, 3);
        system.Emitter.Shape = std::move(mesh);

        // Exercise both allocation growth and a guaranteed in-buffer relocation
        // while live trails exist. Inserting before the system moves its bytes
        // even when the allocator could grow the allocation in place.
        TArray<ParticleSystem> systems;
        systems.Reserve(1);
        systems.Add(std::move(system));
        systems.Reserve(128);
        systems.EmplaceAt(0, 1);
        system = std::move(systems[1]);

        const auto* movedMesh = std::get_if<EmitMesh>(&system.Emitter.Shape);
        ASSERT_NE(movedMesh, nullptr);
        ASSERT_EQ(movedMesh->Triangles.Num(), 1);
        ASSERT_EQ(movedMesh->CumulativeAreas.Num(), 1);
        EXPECT_FLOAT_EQ(movedMesh->TotalArea, 0.5f);
        EXPECT_FLOAT_EQ(movedMesh->CumulativeAreas[0], 0.5f);
        const auto sample = SampleEmissionCombined(system.Emitter.Shape, system.GetRandom());
        EXPECT_FLOAT_EQ(sample.Position.y, 0.0f);
        EXPECT_GE(sample.Position.x, 0.0f);
        EXPECT_GE(sample.Position.z, 0.0f);
        EXPECT_LE(sample.Position.x + sample.Position.z, 1.00001f);
    }

    Entity m_Emitter;
};

TEST_F(ParticleSystemEmitsAndExpiresTest, BurstEmitsThenAllParticlesExpire)
{
    // Phase 1: tick 0.5s. Duration is 0.4s with RateOverTime=50, so the
    // burst budget is ~20 particles. We should have a non-trivial pool.
    RunFrames(/*count=*/30); // 0.5s
    const u32 burstCount = AliveCount();
    EXPECT_GT(burstCount, 5u)
        << "particles did not emit during scene tick — Scene→ParticleSystem update wiring is broken; alive=" << burstCount;

    // Phase 2: tick 5s. Lifetime cap is 0.5s, so by now every particle from
    // the burst must have expired. If alive > 0, the system either leaked
    // a particle or its lifetime-decrement step stopped firing (the
    // pre-fix behaviour: Update early-returns once Playing flips to false).
    RunFrames(/*count=*/300); // 5s
    const u32 endCount = AliveCount();
    EXPECT_EQ(endCount, 0u)
        << "particles did not expire after 5s of ticking; alive=" << endCount
        << " (lifetime cap is 0.5s, so any alive particle is frozen past its deadline)";
}

TEST_F(ParticleSystemEmitsAndExpiresTest, ExpirationAfterStorageGrowthKeepsSurvivorTrail)
{
    ASSERT_NO_FATAL_FAILURE(SeedTrailsAndMoveSystem());
    auto& system = m_Emitter.GetComponent<ParticleSystemComponent>().System;
    system.GetPool().m_Lifetimes[0] = 0.0f;

    RunFrames(1);

    ASSERT_EQ(system.GetAliveCount(), 2u);
    EXPECT_FLOAT_EQ(system.GetPool().m_Positions[0].x, 30.0f);
    const auto& survivorTrail = system.GetTrailData().GetTrail(0);
    ASSERT_GT(survivorTrail.m_Count, 0u);
    EXPECT_FLOAT_EQ(survivorTrail.Get(survivorTrail.m_Count - 1).Position.x, 30.0f);
}

TEST_F(ParticleSystemEmitsAndExpiresTest, CollisionAfterStorageGrowthKeepsSurvivorTrail)
{
    ASSERT_NO_FATAL_FAILURE(SeedTrailsAndMoveSystem());
    auto& system = m_Emitter.GetComponent<ParticleSystemComponent>().System;
    system.CollisionModule.Enabled = true;
    system.CollisionModule.KillOnCollide = true;
    system.GetPool().m_Positions[0].y = -1.0f;

    RunFrames(1);

    ASSERT_EQ(system.GetAliveCount(), 2u);
    EXPECT_FLOAT_EQ(system.GetPool().m_Positions[0].x, 30.0f);
    const auto& survivorTrail = system.GetTrailData().GetTrail(0);
    ASSERT_GT(survivorTrail.m_Count, 0u);
    // Check the oldest point too: recording the new survivor position must not
    // hide a trail that still belongs to the particle removed from slot zero.
    EXPECT_FLOAT_EQ(survivorTrail.Get(survivorTrail.m_Count - 1).Position.x, 30.0f);
}
