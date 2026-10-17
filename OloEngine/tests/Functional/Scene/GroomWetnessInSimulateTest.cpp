// OLO_TEST_LAYER: Functional
#include "OloEnginePCH.h"

// =============================================================================
// GroomWetnessInSimulateTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::OnUpdateSimulation × Scene::UpdateGroomWetness × WaterProbe (#1615).
//   The editor's Simulate mode steps physics, and buoyancy moves bodies through
//   the water there, but it does not run the gameplay schedule that soaks a
//   coat in Play. OnUpdateSimulation therefore advances coat wetness itself, on
//   the frames it steps. Without that call a coat stood in a pond in Simulate
//   stays dry.
//
// Scenario: a coat at the origin in a pond whose surface is above it. Simulate
// soaks it. A paused scene does not advance it. The same coat with the pond
// disabled stays dry: that control proves the water, not the call alone, soaks
// the coat.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr f32 kDt = 1.0f / 60.0f;
} // namespace

class GroomWetnessInSimulateTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // No groom asset: the coat measures from its origin, which is all the
        // waterline needs.
        m_Coat = GetScene().CreateEntity("Coat");
        m_Coat.AddComponent<GroomComponent>();
        auto& wet = m_Coat.AddComponent<GroomWetnessComponent>();
        wet.m_SoakRate = 2.0f;
        wet.m_DryRate = 0.1f;

        m_Pond = GetScene().CreateEntity("Pond");
        m_Pond.GetComponent<TransformComponent>().Translation = { 0.0f, 0.5f, 0.0f };
        auto& water = m_Pond.AddComponent<WaterComponent>();
        water.m_WorldSizeX = 4.0f;
        water.m_WorldSizeZ = 4.0f;
        water.m_WaveAmplitude = 0.0f;
        water.m_WaveSpeed = 0.0f;
    }

    void Simulate(u32 frames)
    {
        const EditorCamera camera;
        for (u32 i = 0; i < frames; ++i)
        {
            GetScene().OnUpdateSimulation(Timestep(kDt), camera);
        }
    }

    [[nodiscard]] f32 Immersion() const
    {
        return m_Coat.GetComponent<GroomWetnessComponent>().m_Immersion;
    }

    Entity m_Coat;
    Entity m_Pond;
};

TEST_F(GroomWetnessInSimulateTest, ACoatInWaterSoaksInSimulateAndAPausedSceneHoldsIt)
{
    ASSERT_FLOAT_EQ(Immersion(), 0.0f);

    // A quarter second at a soak rate of two a second: half soaked.
    Simulate(15u);
    const f32 soaking = Immersion();
    EXPECT_NEAR(soaking, 0.5f, 1.0e-3f) << "Simulate stood the coat in the pond and did not soak it at its rate";

    GetScene().SetPaused(true);
    Simulate(30u);
    EXPECT_FLOAT_EQ(Immersion(), soaking) << "a paused scene advanced the coat's wetness";

    GetScene().SetPaused(false);
    Simulate(30u);
    EXPECT_FLOAT_EQ(Immersion(), 1.0f) << "standing in the water soaks the coat fully";
}

TEST_F(GroomWetnessInSimulateTest, WithThePondDisabledTheCoatStaysDry)
{
    m_Pond.GetComponent<WaterComponent>().m_Enabled = false;
    Simulate(60u);
    EXPECT_FLOAT_EQ(Immersion(), 0.0f) << "the coat soaked with no water under it";
}
