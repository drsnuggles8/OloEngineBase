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
