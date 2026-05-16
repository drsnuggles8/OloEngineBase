#include "OloEnginePCH.h"

// =============================================================================
// CharacterControllerContactCallbackTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Physics3D × character controller contact callback × test-side closure.
//   The character controller's contact path is the only way gameplay code
//   sees physics contacts in OloEngine — rigid-body contacts only get
//   logged. So this callback is the production "OnHit" pipeline for any
//   character (player picks up loot by walking over it, NPC reacts to
//   bumping into wall). A regression here silently breaks every "trigger"
//   in the project.
//
// Scenario: a character controller positioned next to a static box. We
// wire a callback via JoltScene::CreateCharacterController's optional
// callback parameter, drive a velocity toward the box, tick. Assert the
// callback fired with both entity handles populated.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"

#include <atomic>

using namespace OloEngine;
using namespace OloEngine::Functional;

class CharacterControllerContactCallbackTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Ground floor so the character has somewhere to stand.
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // The wall the character will walk into.
        m_Wall = GetScene().CreateEntity("Wall");
        m_Wall.GetComponent<TransformComponent>().Translation = { 2.0f, 0.5f, 0.0f };
        Rigidbody3DComponent wallBody;
        wallBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent wallCol;
        wallCol.m_HalfExtents = { 0.5f, 1.0f, 5.0f };
        m_Wall.AddComponent<BoxCollider3DComponent>(wallCol);
        m_Wall.AddComponent<Rigidbody3DComponent>(wallBody);

        // The character — collider FIRST so the controller's shape resolves.
        m_Character = GetScene().CreateEntity("Player");
        m_Character.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        CapsuleCollider3DComponent capsule;
        capsule.m_Radius = 0.4f;
        capsule.m_HalfHeight = 0.6f;
        m_Character.AddComponent<CapsuleCollider3DComponent>(capsule);
        // We add the CharacterController3DComponent here so the
        // OnComponentAdded hook builds the JoltCharacterController. We then
        // re-create the controller below with our callback wired — Create
        // is idempotent on the entity, but we want to be explicit.
        m_Character.AddComponent<CharacterController3DComponent>();

        EnablePhysics3D();
    }

    Entity m_Character;
    Entity m_Wall;
    std::atomic<u32> m_CallbackHits{ 0 };
    UUID m_OtherEntitySeen{ 0 };
    // Wall-specific record so the assertion below doesn't depend on
    // wall being the *last* callback to fire — if any non-wall contact
    // fires after the wall (e.g. a future change re-enables gravity and
    // the controller settles onto a floor), the wall hit still survives.
    UUID m_WallEntitySeen{ 0 };
};

TEST_F(CharacterControllerContactCallbackTest, CallbackFiresWhenControllerTouchesStaticWall)
{
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);

    // Replace the controller with one that has our test callback wired. The
    // OnComponentAdded hook already built one without a callback, so destroy
    // it first to avoid the "controller already exists" branch.
    joltScene->DestroyCharacterController(m_Character);

    const UUID wallUUID = m_Wall.GetUUID();
    auto controller = joltScene->CreateCharacterController(m_Character,
                                                           [this, wallUUID](Entity self, Entity other)
                                                           {
                                                               (void)self;
                                                               ++m_CallbackHits;
                                                               const UUID otherUUID = other.GetUUID();
                                                               m_OtherEntitySeen = otherUUID;
                                                               if (otherUUID == wallUUID)
                                                                   m_WallEntitySeen = otherUUID;
                                                           });
    ASSERT_TRUE(controller) << "JoltScene refused to recreate the controller";

    // No floor collisions — keep the test focused on the wall contact.
    controller->SetGravityEnabled(false);
    controller->SetControlMovementInAir(true);
    controller->SetLinearVelocity({ 4.0f, 0.0f, 0.0f }); // Toward the wall.

    // 1.0s × 4 m/s = 4m of attempted motion. Wall starts at x=2 and is 1m
    // thick; from start x=0 the controller should hit it within ~0.4s.
    TickFor(/*seconds=*/1.0f);

    EXPECT_GT(m_CallbackHits.load(), 0u)
        << "controller's contact callback never fired even though the controller "
           "drove into a static wall — the JoltCharacterController contact path "
           "(possibly the post-API-change OnContactAdded) is not invoking the "
           "registered callback.";

    // The wall must have been a registered contact — order-independent of
    // any other contacts that may have fired in the same window.
    EXPECT_EQ(m_WallEntitySeen, m_Wall.GetUUID())
        << "callback fired but never recorded a contact with the wall (saw "
           "other="
        << static_cast<u64>(m_OtherEntitySeen)
        << ") — the controller is missing the static-wall collision event.";
}
