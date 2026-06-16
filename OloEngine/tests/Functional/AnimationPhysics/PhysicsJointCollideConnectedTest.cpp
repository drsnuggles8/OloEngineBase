#include "OloEnginePCH.h"

// =============================================================================
// PhysicsJointCollideConnectedTest — Functional Test (issue #308 item 1).
//
// Cross-subsystem seam under test:
//   PhysicsJoint3DComponent::m_CollideConnected (authored ECS data) ×
//   JoltScene::ApplyJointCollisionFilters (shared CollisionGroup +
//   GroupFilterTable) × Physics3D contact generation, driven through a real
//   Scene::OnUpdateRuntime tick. Jolt has no per-constraint "collide connected"
//   flag for two-body constraints, so the flag is realised by placing every
//   body that participates in a no-collide joint in one shared collision group
//   and disabling exactly the authored pairs in a GroupFilterTable.
//
// The behavioural tests use a wide, sign-independent positional signal so a
// broken filter (or none at all) fails by a large margin:
//   * false  → a body falls straight through the body it is jointed to;
//   * true   → the same body is blocked and rests on it (regression guard);
//   * a body shared by two no-collide joints (chain A–B–C) stops colliding
//     with each direct partner but still collides with the indirect one,
//     proving the disabling is pairwise, not whole-group / transitive.
//
// Two further tests round-trip the flag through scene YAML and the save-game
// serializer so an authored value persists through both paths.
//
// Functional-test contract: see docs/testing.md §7, ADR 0001/0002/0003.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class PhysicsJointCollideConnectedTest : public FunctionalTest
{
  protected:
    // Each test builds its own entities in the test body, then calls
    // EnablePhysics3D() (which snapshots whatever exists at call time), so the
    // shared BuildScene stays empty — mirrors PhysicsJoint3DTest.
    void BuildScene() override {}

    Entity MakeBox(const std::string& name, const glm::vec3& pos, BodyType3D type, f32 halfExtent = 0.5f)
    {
        Entity e = GetScene().CreateEntity(name);
        e.GetComponent<TransformComponent>().Translation = pos;
        auto& rb = e.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = type;
        rb.m_Mass = 1.0f;
        rb.m_LinearDrag = 0.0f;
        rb.m_AngularDrag = 0.0f;
        auto& col = e.AddComponent<BoxCollider3DComponent>();
        col.m_HalfExtents = glm::vec3(halfExtent);
        return e;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }
};

// -----------------------------------------------------------------------------
// CollideConnected == false → the dynamic body is no longer blocked by the
// static body it is jointed to: gravity pulls it straight through, and the
// slack distance rope (max 3) finally catches it ~3 m below the anchor. A body
// that still collided would rest on top of the anchor near its start.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJointCollideConnectedTest, CollideConnectedFalseLetsBodyPassThroughConnectedBody)
{
    Entity anchor = MakeBox("Anchor", { 0.0f, 3.0f, 0.0f }, BodyType3D::Static, 0.5f);
    Entity bob = MakeBox("Bob", { 0.0f, 4.0f, 0.0f }, BodyType3D::Dynamic, 0.5f);

    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Distance;
    joint.m_ConnectedEntity = anchor.GetUUID();
    joint.m_MinDistance = 0.0f;
    joint.m_MaxDistance = 3.0f;       // slack: starts at 1 m, catches the fall at 3 m
    joint.m_CollideConnected = false; // the body under test: bodies must NOT collide

    EnablePhysics3D();

    TickFor(4.0f);

    const glm::vec3 p = Pos(bob);
    const f32 dist = glm::distance(p, Pos(anchor));
    // Fell through the anchor (a blocked body rests at y ~ 4.0); the rope then
    // catches it ~3 m below the anchor at y ~ 0.
    EXPECT_LT(p.y, 1.5f) << "body did not pass through its connected body — collision was not disabled; y=" << p.y;
    EXPECT_NEAR(dist, 3.0f, 0.3f) << "distance rope did not catch the fallen body at its max length; dist=" << dist;
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
}

// -----------------------------------------------------------------------------
// CollideConnected == true (the default) → regression guard: the same setup
// must keep the bodies colliding, so the dynamic body lands on the static body
// and rests on top of it (~y = 4.0) instead of falling through. Proves the
// default leaves the long-standing behaviour unchanged.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJointCollideConnectedTest, CollideConnectedTrueKeepsBodiesColliding)
{
    Entity anchor = MakeBox("Anchor", { 0.0f, 3.0f, 0.0f }, BodyType3D::Static, 0.5f);
    Entity bob = MakeBox("Bob", { 0.0f, 4.0f, 0.0f }, BodyType3D::Dynamic, 0.5f);

    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Distance;
    joint.m_ConnectedEntity = anchor.GetUUID();
    joint.m_MinDistance = 0.0f;
    joint.m_MaxDistance = 3.0f;
    // m_CollideConnected left at its true default → bodies must still collide.

    EnablePhysics3D();

    TickFor(4.0f);

    const glm::vec3 p = Pos(bob);
    // Rests on top of the anchor (top at y = 3.5, body half-extent 0.5 → y ~ 4.0);
    // it must not sink through to the rope's catch point at y ~ 0.
    EXPECT_GT(p.y, 3.6f) << "body fell through its connected body — default should keep them colliding; y=" << p.y;
    EXPECT_NEAR(p.y, 4.0f, 0.3f) << "body did not rest on top of its connected body; y=" << p.y;
    EXPECT_TRUE(std::isfinite(p.y));
}

// -----------------------------------------------------------------------------
// Pairwise, not transitive. Chain A–B–C: the hub B is in two joints — A and C
// each rope to B with CollideConnected == false, so A, B and C all land in the
// one shared collision group. A and C are NOT jointed to each other, so their
// collision must stay active even though they share that group. A and C start
// overlapping, resting side by side on a floor; if the pair is still colliding
// they push apart to ~2·halfExtent, but a whole-group / transitive disable would
// leave them overlapping near their start. The ropes to B are deliberately slack
// (and B is high overhead) so the joints only establish group membership — the
// A–C separation is decided by collision alone, not by the constraints.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJointCollideConnectedTest, NoCollideDisablingIsPairwiseNotTransitive)
{
    // Wide static floor for A and C to rest on. It is in no joint, so its
    // collision (different group) stays active and holds the links up.
    Entity floor = MakeBox("Floor", { 0.0f, 0.0f, 0.0f }, BodyType3D::Static, 0.5f);
    floor.GetComponent<BoxCollider3DComponent>().m_HalfExtents = { 5.0f, 0.5f, 5.0f };

    // Shared hub B (in two joints), parked high overhead and out of the way.
    Entity b = MakeBox("Hub", { 0.0f, 6.0f, 0.0f }, BodyType3D::Static, 0.25f);

    // A and C rest on the floor (top at y = 0.5, half-extent 0.5 → y = 1.0),
    // starting overlapping each other on the x axis (centres 0.6 apart).
    Entity a = MakeBox("LinkA", { 0.3f, 1.0f, 0.0f }, BodyType3D::Dynamic, 0.5f);
    Entity c = MakeBox("LinkC", { -0.3f, 1.0f, 0.0f }, BodyType3D::Dynamic, 0.5f);

    // Each link ropes to B with a very slack max length and no-collide, so the
    // joint never goes taut (never lifts the link off the floor) — it exists only
    // to put A, B and C in the shared collision group.
    const auto rope = [](Entity owner, Entity hub)
    {
        auto& j = owner.AddComponent<PhysicsJoint3DComponent>();
        j.m_Type = JointType3D::Distance;
        j.m_ConnectedEntity = hub.GetUUID();
        j.m_MinDistance = 0.0f;
        j.m_MaxDistance = 20.0f;
        j.m_CollideConnected = false;
    };
    rope(a, b);
    rope(c, b);

    EnablePhysics3D();

    TickFor(3.0f);

    const f32 distAC = glm::distance(Pos(a), Pos(c));
    // A and C share the no-collide group (each roped to B) but are not jointed to
    // each other, so their collision stays active and pushes them apart to
    // ~2·halfExtent = 1.0. A transitive / whole-group disable would leave them
    // overlapping near their 0.6 start.
    EXPECT_GT(distAC, 0.8f) << "A and C overlapped — disabling leaked to an unjointed pair (transitive); distAC=" << distAC;
    // Both stayed resting on the floor (the slack ropes never yanked them up).
    EXPECT_NEAR(Pos(a).y, 1.0f, 0.4f) << "A left the floor; y=" << Pos(a).y;
    EXPECT_NEAR(Pos(c).y, 1.0f, 0.4f) << "C left the floor; y=" << Pos(c).y;
    EXPECT_TRUE(std::isfinite(distAC));
}

// -----------------------------------------------------------------------------
// Scene YAML round-trip — an authored m_CollideConnected must survive
// SceneSerializer write → read. Set to the non-default (false) so a dropped or
// unwritten field would read back as the true default and fail.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJointCollideConnectedTest, CollideConnectedSurvivesSceneYAMLRoundTrip)
{
    Entity e = GetScene().CreateEntity("JointYAML");
    auto& j = e.AddComponent<PhysicsJoint3DComponent>();
    j.m_Type = JointType3D::Fixed;
    j.m_ConnectedEntity = UUID(0x1234ABCDULL);
    j.m_CollideConnected = false;

    SceneSerializer serializer(GetSceneRef());
    const std::string yaml = serializer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty()) << "SerializeToYAML produced an empty string.";

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    SceneSerializer restoreSerializer(restored);
    ASSERT_TRUE(restoreSerializer.DeserializeFromYAML(yaml)) << "Scene YAML round-trip failed to deserialize.";

    Entity re = restored->FindEntityByName("JointYAML");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<PhysicsJoint3DComponent>())
        << "PhysicsJoint3DComponent dropped by the scene YAML round-trip";
    EXPECT_FALSE(re.GetComponent<PhysicsJoint3DComponent>().m_CollideConnected)
        << "m_CollideConnected did not survive the scene YAML round-trip";
}

// -----------------------------------------------------------------------------
// Save-game round-trip — the authored m_CollideConnected must survive
// CaptureSceneState → RestoreSceneState (exercises the SaveGameComponentSerializer
// versioned tail). Again set to the non-default (false).
// -----------------------------------------------------------------------------
TEST_F(PhysicsJointCollideConnectedTest, CollideConnectedSurvivesSaveGameRoundTrip)
{
    Entity e = GetScene().CreateEntity("JointSaveGame");
    auto& j = e.AddComponent<PhysicsJoint3DComponent>();
    j.m_Type = JointType3D::Fixed;
    j.m_ConnectedEntity = UUID(0x9988AABBULL);
    j.m_CollideConnected = false;

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("JointSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<PhysicsJoint3DComponent>())
        << "PhysicsJoint3DComponent dropped by the save-game round-trip";
    EXPECT_FALSE(re.GetComponent<PhysicsJoint3DComponent>().m_CollideConnected)
        << "m_CollideConnected did not survive the save-game round-trip";
}
