#include "OloEnginePCH.h"
// OLO_TEST_LAYER: Functional

// =============================================================================
// LocomotionControllerTest — Functional Test (issue #631 part 4).
//
// Cross-subsystem seam under test:
//   Scene tick × LocomotionSystem × AnimationGraphComponent parameters.
//   The locomotion controller must turn character velocity into the graph's
//   blend parameters (speed / local direction / turn), select a gait with
//   hysteresis (no flicker at the boundary), and stride-warp the active
//   state's playback rate so the clip's stride speed tracks ground speed.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/AnimationLayer.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/LocomotionComponent.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/gtc/quaternion.hpp>

using namespace OloEngine;
using namespace OloEngine::Functional;

class LocomotionControllerTest : public FunctionalTest
{
  protected:
    static constexpr const char* kStateName = "Move";

    void BuildScene() override
    {
        m_Character = GetScene().CreateEntity("Character");
        m_Character.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());

        AnimationState state;
        state.Name = kStateName;
        state.Type = AnimationState::MotionType::SingleClip;
        state.Clip = Fixtures::MakeTranslationClip(/*duration=*/1.0f);
        state.Looping = true;
        state.Speed = 1.0f;

        auto stateMachine = Ref<AnimationStateMachine>::Create();
        stateMachine->AddState(state);
        stateMachine->SetDefaultState(kStateName);

        auto graph = Ref<AnimationGraph>::Create();
        AnimationLayer layer;
        layer.Name = "Base";
        layer.StateMachine = stateMachine;
        graph->Layers.push_back(std::move(layer));

        AnimationGraphComponent graphComp;
        graphComp.RuntimeGraph = graph;
        m_Character.AddComponent<AnimationGraphComponent>(std::move(graphComp));

        auto& loco = m_Character.AddComponent<LocomotionComponent>();
        loco.UseDesiredVelocity = true; // steer directly from the test
    }

    void SetDesiredVelocity(const glm::vec3& v)
    {
        m_Character.GetComponent<LocomotionComponent>().DesiredVelocity = v;
    }

    [[nodiscard]] const AnimationParameterSet& Params()
    {
        return m_Character.GetComponent<AnimationGraphComponent>().Parameters;
    }

    Entity m_Character;
};

TEST_F(LocomotionControllerTest, VelocityDrivesSpeedDirectionAndGaitParameters)
{
    SetDesiredVelocity({ 0.0f, 0.0f, 2.0f }); // forward walk
    RunFrames(90);                            // let the smoothing converge

    EXPECT_NEAR(Params().GetFloat("Speed"), 2.0f, 0.05f);
    EXPECT_NEAR(Params().GetFloat("MoveY"), 2.0f / 4.0f, 0.03f) << "forward dir normalized by reference speed";
    EXPECT_NEAR(Params().GetFloat("MoveX"), 0.0f, 0.03f);
    EXPECT_EQ(Params().GetInt("Gait"), 1) << "2 m/s is walking";

    SetDesiredVelocity({ 0.0f, 0.0f, 5.0f });
    RunFrames(90);
    EXPECT_EQ(Params().GetInt("Gait"), 2) << "5 m/s is running";
}

TEST_F(LocomotionControllerTest, GaitHysteresisHoldsInsideTheBand)
{
    // Run first...
    SetDesiredVelocity({ 0.0f, 0.0f, 5.0f });
    RunFrames(90);
    ASSERT_EQ(Params().GetInt("Gait"), 2);

    // ...then hover between RunExit (2.5) and RunEnter (3.0): the gait must
    // HOLD run — dropping to walk here would flicker on re-acceleration.
    SetDesiredVelocity({ 0.0f, 0.0f, 2.7f });
    RunFrames(120);
    EXPECT_EQ(Params().GetInt("Gait"), 2) << "gait flickered inside the hysteresis band";

    // Below RunExit it demotes to walk...
    SetDesiredVelocity({ 0.0f, 0.0f, 2.0f });
    RunFrames(120);
    EXPECT_EQ(Params().GetInt("Gait"), 1);

    // ...and only near-standstill demotes to idle (below WalkExit 0.08).
    SetDesiredVelocity({ 0.0f, 0.0f, 0.0f });
    RunFrames(240);
    EXPECT_EQ(Params().GetInt("Gait"), 0);
}

TEST_F(LocomotionControllerTest, LocalDirectionFollowsEntityHeading)
{
    // Face +X (yaw 90°): a world +X velocity is then pure LOCAL forward.
    auto& transform = m_Character.GetComponent<TransformComponent>();
    transform.SetRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));

    SetDesiredVelocity({ 2.0f, 0.0f, 0.0f });
    RunFrames(90);

    EXPECT_NEAR(Params().GetFloat("MoveY"), 0.5f, 0.05f) << "world +X should be local forward for a +X-facing entity";
    EXPECT_NEAR(Params().GetFloat("MoveX"), 0.0f, 0.05f);
}

TEST_F(LocomotionControllerTest, StrideWarpScalesActiveStatePlaybackRate)
{
    SetDesiredVelocity({ 0.0f, 0.0f, 2.0f }); // walk gait; WalkClipSpeed = 1.4
    RunFrames(90);

    const auto& sm = m_Character.GetComponent<AnimationGraphComponent>().RuntimeGraph->Layers[0].StateMachine;
    const AnimationState* state = sm->GetState(kStateName);
    ASSERT_TRUE(state);
    // 2.0 / 1.4 ≈ 1.43, inside the 1.5 clamp.
    EXPECT_NEAR(state->Speed, 2.0f / 1.4f, 0.05f) << "playback rate not warped to match ground speed";

    // Turning the warp off restores the authored playback rate.
    m_Character.GetComponent<LocomotionComponent>().StrideWarp = false;
    RunFrames(2);
    EXPECT_NEAR(state->Speed, 1.0f, 1e-3f) << "authored playback rate not restored";
}

TEST_F(LocomotionControllerTest, MeasuredVelocityFallsBackToTransformDeltas)
{
    auto& loco = m_Character.GetComponent<LocomotionComponent>();
    loco.UseDesiredVelocity = false; // no controller on this entity → transform deltas

    // Move the entity 2 m/s by hand: 1/30 m per 1/60 s frame... 2 m/s = 2/60 per frame.
    for (int i = 0; i < 120; ++i)
    {
        m_Character.GetComponent<TransformComponent>().Translation += glm::vec3(0.0f, 0.0f, 2.0f / 60.0f);
        RunFrames(1);
    }
    EXPECT_NEAR(Params().GetFloat("Speed"), 2.0f, 0.1f) << "measured speed from transform deltas";
    EXPECT_EQ(Params().GetInt("Gait"), 1);
}
