#include "OloEnginePCH.h"

// =============================================================================
// TriggerEndEventFiresOnSeparationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   JoltCharacterController × trigger-leave bookkeeping. The companion to
//   `TriggerVolumeFiresOverlapEventTest`: not just enter, but EXIT. The
//   PostSimulate code in the controller diff's m_TriggeredBodies (last
//   frame) against m_StillTriggeredBodies (this frame) and fires the
//   callback for any body present in the former but not the latter.
//   That diff is its own state machine — a regression in the swap-and-
//   clear logic would make "OnTriggerExit" never fire. Production
//   gameplay code uses this for "stop applying damage zone effects when
//   the player leaves." Without it, the player gets flame-tagged forever.
//
// Scenario: drive the character into the trigger, then back out. Count
// callback invocations. Expect at least 2 (begin + end). Verify the
// "other" entity reported is the trigger (not the floor) for both.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"

#include <atomic>
#include <unordered_map>
#include <mutex>

using namespace OloEngine;
using namespace OloEngine::Functional;

class TriggerEndEventFiresOnSeparationTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // Trigger volume at x=2, narrow enough that we can walk through it.
        m_Trigger = GetScene().CreateEntity("Trigger");
        m_Trigger.GetComponent<TransformComponent>().Translation = { 2.0f, 1.0f, 0.0f };
        Rigidbody3DComponent triggerBody;
        triggerBody.m_Type = BodyType3D::Static;
        triggerBody.m_IsTrigger = true;
        BoxCollider3DComponent triggerCol;
        triggerCol.m_HalfExtents = { 0.4f, 1.0f, 5.0f }; // narrow on X so we can walk through
        m_Trigger.AddComponent<BoxCollider3DComponent>(triggerCol);
        m_Trigger.AddComponent<Rigidbody3DComponent>(triggerBody);

        m_Character = GetScene().CreateEntity("Player");
        m_Character.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        CapsuleCollider3DComponent capsule;
        capsule.m_Radius = 0.4f;
        capsule.m_HalfHeight = 0.6f;
        m_Character.AddComponent<CapsuleCollider3DComponent>(capsule);
        m_Character.AddComponent<CharacterController3DComponent>();

        EnablePhysics3D();
    }

    Entity m_Trigger;
    Entity m_Character;
    // The character also collides with the floor (CHARACTER layer collides
    // with NON_MOVING by default), so the callback fires for floor contacts
    // too. We need per-other-entity hit counting to assert specifically on
    // the trigger.
    std::mutex m_HitsMutex;
    std::unordered_map<u64, u32> m_HitsByOther;
};

TEST_F(TriggerEndEventFiresOnSeparationTest, EnterAndExitBothInvokeTheCallback)
{
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);

    joltScene->DestroyCharacterController(m_Character);
    auto controller = joltScene->CreateCharacterController(m_Character,
                                                           [this](Entity self, Entity other)
                                                           {
                                                               (void)self;
                                                               std::lock_guard<std::mutex> lock(m_HitsMutex);
                                                               ++m_HitsByOther[static_cast<u64>(other.GetUUID())];
                                                           });
    ASSERT_TRUE(controller);

    controller->SetGravityEnabled(false);
    controller->SetControlMovementInAir(true);

    // Walk forward through the trigger (~1.5s of +x at 4 m/s = 6m
    // travelled — well past the trigger which is at x=2).
    controller->SetLinearVelocity({ 4.0f, 0.0f, 0.0f });
    TickFor(/*seconds=*/1.5f);

    ASSERT_GT(m_Character.GetComponent<TransformComponent>().Translation.x, 4.0f)
        << "character didn't move past the trigger; controller setup is broken";

    // Filter to just the trigger's hits; floor contacts are also expected
    // (CHARACTER ↔ NON_MOVING layer interaction) but not what we're testing.
    u32 triggerHits = 0;
    {
        std::lock_guard<std::mutex> lock(m_HitsMutex);
        const auto it = m_HitsByOther.find(static_cast<u64>(m_Trigger.GetUUID()));
        if (it != m_HitsByOther.end())
        {
            triggerHits = it->second;
        }
    }

    EXPECT_GE(triggerHits, 2u)
        << "trigger callback fired only " << triggerHits
        << " times — expected at least 2 (enter + exit). The controller's "
           "PostSimulate diff between m_TriggeredBodies and m_StillTriggeredBodies "
           "is not detecting trigger separation.";
}
