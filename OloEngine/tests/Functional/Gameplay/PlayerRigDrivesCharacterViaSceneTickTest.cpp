#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// PlayerRigDrivesCharacterViaSceneTickTest — Functional Test (issue #645).
//
// Cross-subsystem seam under test:
//   Scene::SimulateRuntimeStep (SystemScheduler) × Gameplay/PlayerRig ×
//   Physics3D/JoltCharacterController.
//
// The rig's own math is unit-tested (PlayerRigTest.cpp). What only a real tick
// can prove is the SEAM: that the "PlayerRig" node's wish velocity reaches the
// character controller and is integrated by the SAME tick's physics step. That
// depends entirely on the node being ordered before PhysicsKick — and if the
// edge were lost, the character would still move, just one tick late, which no
// assertion on the rig's kernels could ever see.
//
// Every test here drives the rig through its EXTERNAL input path
// (m_UseDeviceInput = false), which is the same path a script, a replay or a
// network input command uses. That is deliberate: it is what makes the rig
// testable with no window at all, and it is the path a headless server runs.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Gameplay/PlayerRig/PlayerRigComponents.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigPresets.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigSystem.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr f32 kFixedDt = 1.0f / 60.0f;
} // namespace

class PlayerRigDrivesCharacterViaSceneTickTest : public FunctionalTest
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

        // Collider FIRST so the controller's shape resolves at construction
        // (same convention as CharacterControllerWalksTest).
        m_Player = GetScene().CreateEntity("Player");
        m_Player.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        CapsuleCollider3DComponent capsule;
        capsule.m_Radius = 0.4f;
        capsule.m_HalfHeight = 0.6f;
        m_Player.AddComponent<CapsuleCollider3DComponent>(capsule);
        m_Player.AddComponent<CharacterController3DComponent>();

        PlayerRigComponent rig = PlayerRigPresets::FirstPersonPlayer();
        rig.m_UseDeviceInput = false; // headless: intent is written by the test
        rig.m_WalkSpeed = 3.0f;
        m_Player.AddComponent<PlayerRigComponent>(rig);

        EnablePhysics3D();
    }

    [[nodiscard]] PlayerRigComponent& Rig()
    {
        return m_Player.GetComponent<PlayerRigComponent>();
    }

    [[nodiscard]] glm::vec3 PlayerPosition()
    {
        return m_Player.GetComponent<TransformComponent>().Translation;
    }

    Entity m_Player;
};

// The seam itself: intent in, motion out, through the real schedule.
TEST_F(PlayerRigDrivesCharacterViaSceneTickTest, ForwardIntentWalksTheCharacterAlongTheLookDirection)
{
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);
    ASSERT_TRUE(joltScene->GetCharacterController(m_Player))
        << "no character controller — the rig has nothing to drive";

    // Yaw 0 means looking down -Z (the engine forward), so "forward" intent
    // must walk toward -Z and nowhere else.
    Rig().m_YawDeg = 0.0f;
    Rig().m_MoveInput = { 0.0f, 1.0f };

    const glm::vec3 start = PlayerPosition();
    TickFor(1.0f, kFixedDt);
    const glm::vec3 end = PlayerPosition();

    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));

    const f32 dz = end.z - start.z;
    EXPECT_LT(dz, -2.0f) << "character did not walk forward; dz=" << dz
                         << " (expected about -3 at 3 m/s for 1 s). dz == 0 means the PlayerRig node "
                            "never reached the character controller; a much smaller magnitude means it "
                            "landed AFTER PhysicsKick and is being integrated a tick late.";
    EXPECT_GT(dz, -3.6f) << "character overshot; dz=" << dz;
    EXPECT_NEAR(end.x, start.x, 0.1f) << "forward intent must not drift sideways";
}

TEST_F(PlayerRigDrivesCharacterViaSceneTickTest, WalkDirectionFollowsTheLookYaw)
{
    // Same intent, different yaw: the movement basis is look-relative, so
    // yawing 90 degrees must turn "forward" into what was "left" (-X).
    Rig().m_YawDeg = 90.0f;
    Rig().m_MoveInput = { 0.0f, 1.0f };

    const glm::vec3 start = PlayerPosition();
    TickFor(1.0f, kFixedDt);
    const glm::vec3 end = PlayerPosition();

    EXPECT_LT(end.x - start.x, -2.0f) << "yaw 90 + forward must walk toward -X; dx=" << (end.x - start.x);
    EXPECT_NEAR(end.z, start.z, 0.1f);
}

TEST_F(PlayerRigDrivesCharacterViaSceneTickTest, ReleasingTheStickStopsTheCharacter)
{
    // The character controller's desired velocity PERSISTS across steps — it is
    // never cleared by Jolt — so the rig must re-assert it every tick, zero
    // included. Miss that and the player slides forever after one nudge, which
    // is invisible in any test that only ever holds the stick down.
    Rig().m_MoveInput = { 0.0f, 1.0f };
    TickFor(0.5f, kFixedDt);

    Rig().m_MoveInput = { 0.0f, 0.0f };
    TickFor(0.25f, kFixedDt); // let the controller settle
    const glm::vec3 afterRelease = PlayerPosition();

    TickFor(1.0f, kFixedDt);
    const glm::vec3 later = PlayerPosition();

    const f32 drift = glm::length(glm::vec2(later.x - afterRelease.x, later.z - afterRelease.z));
    EXPECT_LT(drift, 0.05f) << "character kept sliding after the input was released; drift=" << drift;
}

TEST_F(PlayerRigDrivesCharacterViaSceneTickTest, TravelIsFrameRateIndependent)
{
    // Same simulated duration, two very different tick rates: the distance
    // walked must match. A rig that forgot to scale by dt somewhere would
    // travel four times as far at 240 Hz as at 60 Hz.
    Rig().m_MoveInput = { 0.0f, 1.0f };
    const glm::vec3 startA = PlayerPosition();
    TickFor(1.0f, kFixedDt);
    const f32 distanceAt60 = glm::length(PlayerPosition() - startA);

    // Reset the character in place rather than rebuilding the scene (EnTT
    // recycles ids, and a fresh physics world would not be comparable).
    auto controller = GetScene().GetPhysicsScene()->GetCharacterController(m_Player);
    ASSERT_TRUE(controller);
    controller->SetTranslation({ 0.0f, 1.0f, 0.0f });
    controller->SetLinearVelocity(glm::vec3(0.0f));
    TickFor(0.2f, kFixedDt); // settle onto the floor after the teleport

    const glm::vec3 startB = PlayerPosition();
    TickFor(1.0f, 1.0f / 240.0f);
    const f32 distanceAt240 = glm::length(PlayerPosition() - startB);

    EXPECT_NEAR(distanceAt240, distanceAt60, 0.25f)
        << "distance walked in 1 s differs by tick rate: 60 Hz=" << distanceAt60
        << " 240 Hz=" << distanceAt240;
}

TEST_F(PlayerRigDrivesCharacterViaSceneTickTest, JumpIsEdgeTriggeredAndConsumedOnce)
{
    // A jump request is a one-shot: the rig must consume it, or a script that
    // set it once would launch the player again on every subsequent landing.
    TickFor(0.3f, kFixedDt); // settle on the ground
    const f32 groundY = PlayerPosition().y;

    Rig().m_JumpInput = true;
    RunFrames(1, kFixedDt);
    EXPECT_FALSE(Rig().m_JumpInput) << "jump intent must be consumed by the tick that acted on it";

    // Rise, then come back down under gravity and stay down.
    ASSERT_TRUE(TickUntil([&]
                          { return PlayerPosition().y > groundY + 0.3f; }, 1.0f, kFixedDt))
        << "character never left the ground; peak y=" << PlayerPosition().y << " ground y=" << groundY;

    TickFor(3.0f, kFixedDt);
    EXPECT_NEAR(PlayerPosition().y, groundY, 0.2f) << "character never landed again";

    // …and does NOT jump a second time without a fresh request.
    const f32 settledY = PlayerPosition().y;
    TickFor(1.0f, kFixedDt);
    EXPECT_NEAR(PlayerPosition().y, settledY, 0.2f) << "character jumped again without a new request";
}

TEST_F(PlayerRigDrivesCharacterViaSceneTickTest, LookIntentIsConsumedOnceNotRepeatedEveryTick)
{
    // m_LookInput is a DISPLACEMENT. The system must zero it after applying it,
    // or a single mouse flick would keep spinning the player for as long as
    // nobody wrote the field again — the bug that turns a 30 degree turn into
    // an endless pirouette at high frame rates.
    Rig().m_LookSensitivity = 1.0f;
    Rig().m_YawDeg = 0.0f;
    Rig().m_LookInput = { -30.0f, 0.0f };

    RunFrames(1, kFixedDt);
    EXPECT_NEAR(Rig().m_YawDeg, 30.0f, 1e-3f);
    EXPECT_NEAR(Rig().m_LookInput.x, 0.0f, 1e-6f) << "look delta must be consumed";

    RunFrames(10, kFixedDt);
    EXPECT_NEAR(Rig().m_YawDeg, 30.0f, 1e-3f) << "yaw kept drifting after the flick ended";
}

TEST_F(PlayerRigDrivesCharacterViaSceneTickTest, FirstPersonBodyYawsWithTheLook)
{
    // The first-person preset sets m_YawBodyWithLook, so the capsule (and
    // anything parented to it — arms, a weapon) must face where the player is
    // looking. Going through the controller matters: writing the transform
    // directly would be overwritten by the post-physics sync.
    Rig().m_YawDeg = 90.0f;
    TickFor(0.2f, kFixedDt);

    const glm::quat bodyRotation = m_Player.GetComponent<TransformComponent>().GetRotation();
    const glm::vec3 bodyForward = bodyRotation * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 lookForward = PlayerRigSystem::YawRotation(90.0f) * glm::vec3(0.0f, 0.0f, -1.0f);

    EXPECT_GT(glm::dot(bodyForward, lookForward), 0.99f)
        << "body facing did not follow the look yaw";
}
