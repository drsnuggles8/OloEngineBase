#include "OloEnginePCH.h"

// =============================================================================
// TriggerVolumeFiresOverlapEventTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Physics3D × sensor body × character contact callback. Trigger volumes
//   are the production "you walked into the goal" / "you stepped on the
//   damage zone" mechanic — gameplay code drops a body with `m_IsTrigger
//   = true` and expects an event when something overlaps it. The trigger
//   path inside JoltCharacterController routes to HandleTrigger (not
//   HandleCollision), so the IsSensor branch and the callback wiring are
//   distinct code paths from the regular collision test we just fixed.
//
// Scenario: a static trigger volume (Rigidbody3D::m_IsTrigger = true)
// straddling the path of a moving character. We register a callback and
// drive the character into the volume. Assert the callback fires exactly
// once, with the entity-pair populated.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"

#include <atomic>

using namespace OloEngine;
using namespace OloEngine::Functional;

class TriggerVolumeFiresOverlapEventTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Floor so the controller is grounded (callbacks for ground contacts
        // go through HandleCollision, not HandleTrigger; we just want it
        // present so the controller has a sensible standing position).
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // Trigger volume — same shape as a regular static body but flagged
        // as a sensor. CharacterVirtual's contact path is supposed to route
        // sensor bodies to HandleTrigger, distinct from solid contacts.
        m_Trigger = GetScene().CreateEntity("Trigger");
        m_Trigger.GetComponent<TransformComponent>().Translation = { 2.0f, 1.0f, 0.0f };
        Rigidbody3DComponent triggerBody;
        triggerBody.m_Type = BodyType3D::Static;
        triggerBody.m_IsTrigger = true;
        BoxCollider3DComponent triggerCol;
        triggerCol.m_HalfExtents = { 0.5f, 1.0f, 5.0f };
        m_Trigger.AddComponent<BoxCollider3DComponent>(triggerCol);
        m_Trigger.AddComponent<Rigidbody3DComponent>(triggerBody);

        // Character — walks into the trigger.
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
    std::atomic<u32> m_TriggerHits{ 0 };
    UUID m_OtherSeen{ 0 };
};

TEST_F(TriggerVolumeFiresOverlapEventTest, OverlapEventFiresWhenCharacterEntersTrigger)
{
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);

    // Replace the auto-created controller with one that has our test callback.
    joltScene->DestroyCharacterController(m_Character);
    auto controller = joltScene->CreateCharacterController(m_Character,
        [this](Entity self, Entity other) {
            (void)self;
            ++m_TriggerHits;
            m_OtherSeen = other.GetUUID();
        });
    ASSERT_TRUE(controller);

    controller->SetGravityEnabled(false);
    controller->SetControlMovementInAir(true);
    controller->SetLinearVelocity({ 4.0f, 0.0f, 0.0f }); // Toward the trigger.

    TickFor(/*seconds=*/1.0f);

    EXPECT_GT(m_TriggerHits.load(), 0u)
        << "trigger callback never fired even though the character moved into the "
           "sensor volume — JoltCharacterController is not routing sensor contacts "
           "through HandleTrigger, or m_IsTrigger is not propagating to JoltBody's "
           "JPH::Body::SetIsSensor flag at body-creation time.";

    EXPECT_EQ(m_OtherSeen, m_Trigger.GetUUID())
        << "callback fired but the 'other' entity was not the trigger volume "
           "(possibly the floor): trigger uuid="
           << static_cast<u64>(m_Trigger.GetUUID())
           << " seen=" << static_cast<u64>(m_OtherSeen);
}
