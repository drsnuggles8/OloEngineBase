#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// CameraRigSpringArmViaSceneTickTest — Functional Test (issue #645).
//
// Cross-subsystem seam under test:
//   Scene::SimulateRuntimeStep (SystemScheduler) × Gameplay/PlayerRig ×
//   Physics3D scene queries (JoltScene::CastRay + EntityExclusionBodyFilter).
//
// The spring arm's arithmetic is unit-tested (PlayerRigTest.cpp). Everything
// here needs a real physics world, because the interesting failures are all
// about what the boom's raycast actually HITS:
//
//   * The boom must shorten when a wall comes between camera and target, and
//     extend again when it clears.
//   * The boom must NOT collide with the player's own body. The pivot sits
//     INSIDE the player's capsule, and Jolt treats a convex shape as solid, so
//     an unfiltered probe reports a hit at distance 0 and the camera collapses
//     onto the character's head every single frame. That exclusion goes through
//     the body's user data — which is why the character controller's inner body
//     has to carry the entity UUID (it did not, before this issue).
//   * The camera must observe the target's FINAL post-physics pose. A camera
//     placed from a pre-physics pose still "follows"; it just trails by a tick,
//     which reads as judder and which a steady-state position check cannot see.
//     The moving-target test below is the one that catches it.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Gameplay/PlayerRig/PlayerRigComponents.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigPresets.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr f32 kFixedDt = 1.0f / 60.0f;
    constexpr f32 kBoomLength = 4.0f;
    constexpr f32 kProbeRadius = 0.25f;
    constexpr f32 kMinBoomLength = 0.5f;
    // Wall centre and half-thickness: with the pivot at the origin's height and
    // yaw 0, the boom points along +Z, so a wall at z = +2 is hit at z = 1.9.
    constexpr f32 kWallCentreZ = 2.0f;
    constexpr f32 kWallHalfThickness = 0.1f;
} // namespace

class CameraRigSpringArmViaSceneTickTest : public FunctionalTest
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

        m_Player = GetScene().CreateEntity("Player");
        m_Player.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        CapsuleCollider3DComponent capsule;
        capsule.m_Radius = 0.4f;
        capsule.m_HalfHeight = 0.6f;
        m_Player.AddComponent<CapsuleCollider3DComponent>(capsule);
        m_Player.AddComponent<CharacterController3DComponent>();

        PlayerRigComponent playerRig = PlayerRigPresets::ThirdPersonPlayer();
        playerRig.m_UseDeviceInput = false;
        playerRig.m_WalkSpeed = 3.0f;
        playerRig.m_YawDeg = 0.0f;
        playerRig.m_PitchDeg = 0.0f; // level, so the boom lies along +Z
        m_Player.AddComponent<PlayerRigComponent>(playerRig);

        // Camera is a ROOT entity: the rig writes an absolute world pose, so
        // parenting it to the player would apply the player transform twice.
        m_Camera = GetScene().CreateEntity("Camera");
        m_Camera.AddComponent<CameraComponent>();

        CameraRigComponent cameraRig;
        cameraRig.m_Target = m_Player.GetUUID();
        cameraRig.m_PivotOffset = { 0.0f, 0.0f, 0.0f }; // pivot exactly on the player origin
        cameraRig.m_BoomLength = kBoomLength;
        cameraRig.m_CollisionEnabled = true;
        cameraRig.m_ProbeRadius = kProbeRadius;
        cameraRig.m_MinBoomLength = kMinBoomLength;
        cameraRig.m_BoomReturnSpeed = 6.0f;
        cameraRig.m_PositionSmoothTime = 0.0f; // rigid, so assertions are exact
        m_Camera.AddComponent<CameraRigComponent>(cameraRig);

        EnablePhysics3D();
    }

    // A thin static wall spanning the boom's path, created after the physics
    // world is up (the runtime-add path — Scene::OnComponentAdded builds the
    // body immediately once physics is running).
    Entity SpawnWall()
    {
        auto wall = GetScene().CreateEntity("Wall");
        wall.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, kWallCentreZ };
        BoxCollider3DComponent col;
        col.m_HalfExtents = { 5.0f, 5.0f, kWallHalfThickness };
        wall.AddComponent<BoxCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Static;
        wall.AddComponent<Rigidbody3DComponent>(body);
        return wall;
    }

    [[nodiscard]] CameraRigComponent& Rig()
    {
        return m_Camera.GetComponent<CameraRigComponent>();
    }

    [[nodiscard]] glm::vec3 CameraPosition()
    {
        return m_Camera.GetComponent<TransformComponent>().Translation;
    }

    [[nodiscard]] glm::vec3 PlayerPosition()
    {
        return m_Player.GetComponent<TransformComponent>().Translation;
    }

    Entity m_Player;
    Entity m_Camera;
};

// The exclusion filter earning its keep. This is the assertion that fails if
// the character's inner body loses its entity UUID again: the probe would then
// hit the player's own capsule at distance 0 and floor the boom at the minimum.
TEST_F(CameraRigSpringArmViaSceneTickTest, BoomDoesNotCollideWithThePlayersOwnBody)
{
    TickFor(0.3f, kFixedDt);

    EXPECT_NEAR(Rig().m_CurrentBoomLength, kBoomLength, 1e-3f)
        << "the boom collapsed with nothing but the player in front of it — the probe is hitting "
           "the player's own capsule. Check that the camera rig excludes the target AND that the "
           "character controller's inner Jolt body carries the entity UUID as user data.";

    // …and the camera really is a full boom behind the player, along +Z.
    const glm::vec3 offset = CameraPosition() - PlayerPosition();
    EXPECT_NEAR(offset.z, kBoomLength, 0.05f);
    EXPECT_NEAR(offset.x, 0.0f, 0.05f);
    EXPECT_NEAR(offset.y, 0.0f, 0.05f);
}

TEST_F(CameraRigSpringArmViaSceneTickTest, BoomShortensToTheHitDistanceWhenAWallIsBetweenCameraAndTarget)
{
    TickFor(0.3f, kFixedDt);
    ASSERT_NEAR(Rig().m_CurrentBoomLength, kBoomLength, 1e-3f) << "precondition: boom starts fully extended";

    SpawnWall();
    RunFrames(2, kFixedDt); // pull-in is instant, but give the body a tick to exist

    // Pivot is at the player origin (y = 1); the wall's near face is at
    // z = kWallCentreZ - kWallHalfThickness, so the probe should stop one
    // probe-radius short of it.
    const f32 expected = (kWallCentreZ - kWallHalfThickness) - kProbeRadius;
    EXPECT_NEAR(Rig().m_CurrentBoomLength, expected, 0.15f)
        << "boom did not pull in to the wall; current=" << Rig().m_CurrentBoomLength
        << " expected about " << expected;
    EXPECT_LT(CameraPosition().z, kWallCentreZ - kWallHalfThickness)
        << "the camera ended up on the far side of the wall";
}

TEST_F(CameraRigSpringArmViaSceneTickTest, BoomRestoresAtTheReturnRateOnceTheObstructionClears)
{
    Entity wall = SpawnWall();
    TickFor(0.3f, kFixedDt);
    const f32 pulledIn = Rig().m_CurrentBoomLength;
    ASSERT_LT(pulledIn, kBoomLength - 1.0f) << "precondition: the wall pulled the boom in";

    GetScene().DestroyEntity(wall);

    // Rate-limited extension: at 6 m/s it must NOT be back at full length one
    // tick later (that would be a pop), but it must get there within a second.
    RunFrames(1, kFixedDt);
    EXPECT_LT(Rig().m_CurrentBoomLength, pulledIn + 0.5f)
        << "boom snapped back instead of easing out at m_BoomReturnSpeed";
    EXPECT_GT(Rig().m_CurrentBoomLength, pulledIn)
        << "boom did not start extending after the obstruction was removed";

    ASSERT_TRUE(TickUntil([&]
                          { return Rig().m_CurrentBoomLength > kBoomLength - 1e-3f; }, 2.0f, kFixedDt))
        << "boom never returned to full length; current=" << Rig().m_CurrentBoomLength;
}

TEST_F(CameraRigSpringArmViaSceneTickTest, CameraKeepsUpWithAMovingTargetWithinTheSameTick)
{
    // The camera node runs LAST — after the physics fence and the world-matrix
    // compose — so it must see the pose the player ended this tick at, not the
    // one it started from. A rig ordered before the fence still "follows"; the
    // offset just lags by one tick's travel, which at 3 m/s and 1/60 s is 5 cm.
    // The tolerance below is deliberately tighter than that.
    TickFor(0.3f, kFixedDt);
    m_Player.GetComponent<PlayerRigComponent>().m_MoveInput = { 0.0f, 1.0f };

    for (int i = 0; i < 60; ++i)
    {
        RunFrames(1, kFixedDt);
        const glm::vec3 offset = CameraPosition() - PlayerPosition();
        EXPECT_NEAR(offset.z, kBoomLength, 0.02f)
            << "camera lagged the moving target at frame " << i
            << " — the CameraRig node is reading a stale (pre-physics) target pose";
    }

    // Sanity: the player genuinely moved, so the loop above wasn't vacuous.
    EXPECT_LT(PlayerPosition().z, -2.0f);
}

TEST_F(CameraRigSpringArmViaSceneTickTest, ZeroBoomLengthPutsTheCameraAtTheEyePivot)
{
    // First person is not a mode, it is m_BoomLength == 0 — so the same
    // component with the boom zeroed must place the camera exactly at the
    // pivot, with the collision probe having nothing to do.
    Rig().m_BoomLength = 0.0f;
    Rig().m_PivotOffset = { 0.0f, 0.7f, 0.0f };
    Rig().m_Initialized = false; // re-adopt rather than easing from the old pose

    SpawnWall(); // an obstruction must make no difference at zero length
    TickFor(0.3f, kFixedDt);

    EXPECT_NEAR(Rig().m_CurrentBoomLength, 0.0f, 1e-4f);
    const glm::vec3 offset = CameraPosition() - PlayerPosition();
    EXPECT_NEAR(offset.x, 0.0f, 1e-3f);
    EXPECT_NEAR(offset.y, 0.7f, 1e-3f);
    EXPECT_NEAR(offset.z, 0.0f, 1e-3f);
}

TEST_F(CameraRigSpringArmViaSceneTickTest, PitchOrbitsTheCameraAroundThePivotWithoutChangingItsDistance)
{
    // Collision off: this test is about the orbit geometry, and a 45-degree
    // down-swing puts the camera under the floor, where the probe would
    // (correctly) pull the arm in and mask the property being measured.
    Rig().m_CollisionEnabled = false;
    TickFor(0.3f, kFixedDt);

    // Looking UP swings a third-person camera down and back; looking DOWN
    // swings it up. Either way the arm length is unchanged — that separation
    // between orbit and boom is what "spring arm" means.
    m_Player.GetComponent<PlayerRigComponent>().m_PitchDeg = 45.0f;
    TickFor(0.1f, kFixedDt);

    const glm::vec3 offsetUp = CameraPosition() - PlayerPosition();
    EXPECT_NEAR(glm::length(offsetUp), kBoomLength, 0.05f) << "pitch changed the boom length";
    EXPECT_LT(offsetUp.y, -1.0f) << "looking up must place the camera BELOW the pivot";

    m_Player.GetComponent<PlayerRigComponent>().m_PitchDeg = -45.0f;
    TickFor(0.1f, kFixedDt);

    const glm::vec3 offsetDown = CameraPosition() - PlayerPosition();
    EXPECT_NEAR(glm::length(offsetDown), kBoomLength, 0.05f);
    EXPECT_GT(offsetDown.y, 1.0f) << "looking down must place the camera ABOVE the pivot";
}

TEST_F(CameraRigSpringArmViaSceneTickTest, AnUnresolvableTargetLeavesTheCameraAlone)
{
    TickFor(0.3f, kFixedDt);
    const glm::vec3 placed = CameraPosition();

    // A dangling UUID (a deleted target, a prefab authored against another
    // scene) must be inert, not a teleport to the world origin.
    Rig().m_Target = UUID(0xDEADBEEFULL);
    TickFor(0.3f, kFixedDt);

    EXPECT_NEAR(CameraPosition().x, placed.x, 1e-4f);
    EXPECT_NEAR(CameraPosition().y, placed.y, 1e-4f);
    EXPECT_NEAR(CameraPosition().z, placed.z, 1e-4f);
}
