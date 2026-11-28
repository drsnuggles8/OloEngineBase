#include "OloEnginePCH.h"

// =============================================================================
// HierarchyChildOnDestroyedParentTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene hierarchy × entity destruction × physics. Gameplay code spawns
//   parent-child structures (vehicle + turret, character + held items) and
//   destroys them at runtime. The engine has to choose: when the parent is
//   destroyed, are children also destroyed, orphaned, or left dangling? A
//   bug here is "destroying the car leaves the headlight as an invisible
//   ghost entity that still ticks every frame" — invisible to the player,
//   but a memory/perf leak that grows over time.
//
// Scenario: parent + child set up via Entity::SetParent. Parent has physics,
// child does not. Tick once to confirm both alive. Destroy the parent.
// Tick again. Assert one of the well-defined outcomes (child either
// orphaned with parent=null, or child destroyed too) — the *undefined*
// outcome is "child still has a non-null parent UUID pointing at a dead
// entity," which is the bug class.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"

#include <box2d/box2d.h>

#include <array>
#include <cmath>
#include <limits>

using namespace OloEngine;
using namespace OloEngine::Functional;

class HierarchyChildOnDestroyedParentTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& fb = floor.AddComponent<Rigidbody3DComponent>();
        fb.m_Type = BodyType3D::Static;
        auto& fc = floor.AddComponent<BoxCollider3DComponent>();
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };

        // Parent — physics body, will be destroyed mid-test.
        m_Parent = GetScene().CreateEntity("Parent");
        m_Parent.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.5f;
        m_Parent.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        m_Parent.AddComponent<Rigidbody3DComponent>(body);

        // Child — no physics, parented to the parent.
        m_Child = GetScene().CreateEntity("Child");
        m_Child.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        m_Child.SetParent(m_Parent);
        m_ChildUUID = m_Child.GetUUID();

        EnablePhysics3D();
    }

    Entity m_Parent;
    Entity m_Child;
    UUID m_ChildUUID;
};

TEST_F(HierarchyChildOnDestroyedParentTest, ChildDoesNotDanglePointingAtDeadParent)
{
    // Sanity: hierarchy is wired before any destruction.
    {
        ASSERT_TRUE(m_Child.GetParent());
        EXPECT_EQ(m_Child.GetParent().GetUUID(), m_Parent.GetUUID());
    }

    // Tick to confirm the world is alive and the parent moved.
    RunFrames(/*count=*/15); // 0.25s
    ASSERT_LT(m_Parent.GetComponent<TransformComponent>().Translation.y, 5.0f);

    // Destroy the parent mid-tick.
    GetScene().DestroyEntity(m_Parent);
    m_Parent = Entity{};

    // Tick more — gives the engine a frame to process any deferred cleanup.
    RunFrames(/*count=*/15);

    // The child must end up in one of two well-defined states:
    //   (a) destroyed alongside the parent, OR
    //   (b) orphaned: still alive but with no parent (parent UUID == 0)
    auto childOpt = GetScene().TryGetEntityWithUUID(m_ChildUUID);

    if (!childOpt)
    {
        // (a) Recursive destroy — fine. Document via SUCCEED.
        SUCCEED() << "Child was destroyed when parent was destroyed (recursive teardown).";
        return;
    }

    Entity child = *childOpt;
    Entity childParent = child.GetParent();

    // Child is still alive — must be orphaned, not pointing at the dead parent.
    EXPECT_FALSE(childParent)
        << "child still references its destroyed parent — dangling parent UUID is the bug class.";

    // Child's transform should still be readable (no NaN, no UAF).
    const glm::vec3 childPos = child.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(childPos.x) && std::isfinite(childPos.y) && std::isfinite(childPos.z))
        << "orphaned child transform is corrupted after parent destruction";
}

class ParentedPhysicsPlacementTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Rig = GetScene().CreateEntity("Moving Rig");
        auto& rig = m_Rig.GetComponent<TransformComponent>();
        rig.Translation = { 20.0f, 30.0f, 0.0f };
        rig.SetRotationEuler({ 0.0f, 0.0f, glm::half_pi<f32>() });
        rig.Scale = glm::vec3(2.0f);

        for (sizet i = 0; i < 2; ++i)
        {
            const bool parented = i == 0;
            const auto place = [&](const char* name, f32 z)
            {
                Entity entity = GetScene().CreateEntity(name);
                auto& transform = entity.GetComponent<TransformComponent>();
                if (parented)
                {
                    transform.Translation = { 3.0f, 2.0f, z };
                    entity.SetParent(m_Rig);
                }
                else
                {
                    transform.Translation = { 16.0f, 36.0f, 2.0f * z };
                    transform.SetRotationEuler({ 0.0f, 0.0f, glm::half_pi<f32>() });
                    transform.Scale = glm::vec3(2.0f);
                }
                return entity;
            };
            m_2D[i] = place("Box2D Body", 0.0f);
            m_2D[i].AddComponent<Rigidbody2DComponent>().Type = Rigidbody2DComponent::BodyType::Dynamic;
            m_3D[i] = place("Jolt Body", 2.0f);
            m_3D[i].AddComponent<BoxCollider3DComponent>();
            auto& body = m_3D[i].AddComponent<Rigidbody3DComponent>();
            body.m_Type = BodyType3D::Dynamic;
            body.m_IsTrigger = true;
        }

        EnablePhysics2D();
        EnablePhysics3D();
    }

    Entity m_Rig;
    std::array<Entity, 2> m_2D;
    std::array<Entity, 2> m_3D;
};

TEST_F(ParentedPhysicsPlacementTest, Box2DSpawnsAtTheWorldPoseAndKeepsWorldAuthority)
{
    const auto child = m_2D[0].GetComponent<Rigidbody2DComponent>().RuntimeBody;
    const auto twin = m_2D[1].GetComponent<Rigidbody2DComponent>().RuntimeBody;
    ASSERT_TRUE(b2Body_IsValid(child));
    ASSERT_TRUE(b2Body_IsValid(twin));
    EXPECT_NEAR(b2Body_GetPosition(child).x, 16.0f, 1.0e-4f);
    EXPECT_NEAR(b2Body_GetPosition(child).y, 36.0f, 1.0e-4f);
    EXPECT_NEAR(b2Rot_GetAngle(b2Body_GetRotation(child)), b2Rot_GetAngle(b2Body_GetRotation(twin)), 1.0e-4f);

    m_Rig.GetComponent<TransformComponent>().Translation.x += 50.0f;
    RunFrames(3);
    EXPECT_NEAR(b2Body_GetPosition(child).x, b2Body_GetPosition(twin).x, 1.0e-4f);
    EXPECT_NEAR(b2Body_GetPosition(child).y, b2Body_GetPosition(twin).y, 1.0e-4f);
    EXPECT_NEAR(GetScene().GetWorldPosition(m_2D[0]).x, b2Body_GetPosition(child).x, 1.0e-4f);
}

TEST_F(ParentedPhysicsPlacementTest, JoltSpawnsAtTheWorldPoseAndKeepsWorldAuthority)
{
    auto* physics = GetScene().GetPhysicsScene();
    const Ref<JoltBody> child = physics->GetBody(m_3D[0]);
    const Ref<JoltBody> twin = physics->GetBody(m_3D[1]);
    ASSERT_TRUE(child && twin);
    EXPECT_NEAR(glm::length(child->GetPosition() - glm::vec3(16.0f, 36.0f, 4.0f)), 0.0f, 1.0e-4f);
    EXPECT_NEAR(std::abs(glm::dot(child->GetRotation(), twin->GetRotation())), 1.0f, 1.0e-4f);

    m_Rig.GetComponent<TransformComponent>().Translation.x += 50.0f;
    RunFrames(3);
    EXPECT_NEAR(glm::length(child->GetPosition() - twin->GetPosition()), 0.0f, 1.0e-4f);
    EXPECT_NEAR(glm::length(GetScene().GetWorldPosition(m_3D[0]) - child->GetPosition()), 0.0f, 1.0e-4f);
}

TEST_F(ParentedPhysicsPlacementTest, JoltRefusesABodyUnderANonFiniteParent)
{
    // A NaN parent frame has no world pose (issue #1797). Before the guard,
    // ComputeWorldTRS handed back a NaN position and Jolt created the body there.
    m_Rig.GetComponent<TransformComponent>().Scale.x = std::numeric_limits<f32>::quiet_NaN();
    Entity entity = GetScene().CreateEntity("Under NaN");
    entity.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
    entity.SetParent(m_Rig);
    entity.AddComponent<BoxCollider3DComponent>();
    entity.AddComponent<Rigidbody3DComponent>().m_Type = BodyType3D::Dynamic;
    ASSERT_FALSE(Math::IsFinite(GetScene().ComputeWorldTRS(entity).Position)) << "the fixture must produce a non-finite pose";

    const Ref<JoltBody> body = GetScene().GetPhysicsScene()->CreateBody(entity);
    EXPECT_FALSE(body && body->IsValid());
}

TEST_F(ParentedPhysicsPlacementTest, PhysicsChildrenUseTheCurrentBodyPoseOfTheirSimulatedAncestor)
{
    Entity grandparent = GetScene().CreateEntity("Nonuniform Frame");
    grandparent.GetComponent<TransformComponent>().Scale = { 2.0f, 1.0f, 1.0f };
    m_Rig.SetParent(grandparent);
    m_Rig.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, 0.0f, glm::quarter_pi<f32>() });
    m_Rig.AddComponent<BoxCollider3DComponent>();
    Rigidbody3DComponent rigSettings;
    rigSettings.m_Type = BodyType3D::Dynamic;
    rigSettings.m_DisableGravity = true;
    rigSettings.m_InitialLinearVelocity = { 12.0f, 0.0f, 0.0f };
    m_Rig.AddComponent<Rigidbody3DComponent>(rigSettings);
    auto* physics = GetScene().GetPhysicsScene();
    const Ref<JoltBody> parent = physics->GetBody(m_Rig);
    const Ref<JoltBody> joltChild = physics->GetBody(m_3D[0]);
    const auto boxChild = m_2D[0].GetComponent<Rigidbody2DComponent>().RuntimeBody;
    ASSERT_TRUE(parent && joltChild && b2Body_IsValid(boxChild));

    RunFrames(3);
    EXPECT_GT(parent->GetPosition().x, 40.2f);
    EXPECT_NEAR(glm::length(GetScene().GetWorldPosition(m_3D[0]) - joltChild->GetPosition()), 0.0f, 1.0e-4f);
    EXPECT_NEAR(GetScene().GetWorldPosition(m_2D[0]).x, b2Body_GetPosition(boxChild).x, 1.0e-4f);
    EXPECT_NEAR(GetScene().GetWorldPosition(m_2D[0]).y, b2Body_GetPosition(boxChild).y, 1.0e-4f);
}

TEST_F(ParentedPhysicsPlacementTest, JoltWritebackPreservesAnAuthoredMirror)
{
    Entity parent = GetScene().CreateEntity("Mirror Rig");
    const std::array scales{ glm::vec3(-1.0f, 1.0f, 1.0f),
                             glm::vec3(-1.0f, 0.0f, 1.0f),
                             glm::vec3(0.0f, 1.0f, -1.0f) };
    for (sizet index = 0; index < scales.size(); ++index)
    {
        SCOPED_TRACE(index);
        Entity child = GetScene().CreateEntity("Mirrored Body");
        child.SetParent(parent);
        auto& transform = child.GetComponent<TransformComponent>();
        transform.Scale = scales[index];
        transform.Translation.z = 10.0f * static_cast<f32>(index);
        child.AddComponent<CapsuleCollider3DComponent>();
        Rigidbody3DComponent settings;
        settings.m_Type = BodyType3D::Dynamic;
        settings.m_DisableGravity = true;
        child.AddComponent<Rigidbody3DComponent>(settings);
        ASSERT_TRUE(GetScene().GetPhysicsScene()->GetBody(child));
        const glm::mat4 before = GetScene().ComputeWorldTransform(child);

        RunFrames(2);
        const glm::mat4 after = GetScene().GetWorldTransform(child);
        for (glm::length_t axis = 0; axis < 4; ++axis)
            EXPECT_NEAR(glm::length(glm::vec3(before[axis] - after[axis])), 0.0f, 1.0e-4f) << axis;
    }
}

TEST_F(ParentedPhysicsPlacementTest, AuthoredBox2DTeleportUsesTheSameWorldPoseAsTheRootTwin)
{
    m_2D[0].GetComponent<TransformComponent>().Translation = { 5.0f, 2.0f, 0.0f };
    m_2D[1].GetComponent<TransformComponent>().Translation.y = 40.0f;
    GetScene().ApplyAuthoredPhysics2DPose(m_2D[0]);
    GetScene().ApplyAuthoredPhysics2DPose(m_2D[1]);
    const b2Vec2 child = b2Body_GetPosition(m_2D[0].GetComponent<Rigidbody2DComponent>().RuntimeBody);
    const b2Vec2 twin = b2Body_GetPosition(m_2D[1].GetComponent<Rigidbody2DComponent>().RuntimeBody);
    EXPECT_NEAR(child.x, twin.x, 1.0e-4f);
    EXPECT_NEAR(child.y, twin.y, 1.0e-4f);
}

TEST_F(ParentedPhysicsPlacementTest, JoltOffsetsFollowTheFullShearedWorldBasis)
{
    Entity rig = GetScene().CreateEntity("Shear Rig");
    rig.GetComponent<TransformComponent>().Scale.x = 2.0f;
    for (const bool compound : { false, true })
    {
        SCOPED_TRACE(compound);
        Entity child = GetScene().CreateEntity("Offset Collider");
        child.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, 0.0f, glm::quarter_pi<f32>() });
        child.SetParent(rig);
        const glm::vec3 offset(0.0f, 1.0f, 0.0f);
        child.AddComponent<BoxCollider3DComponent>().m_Offset = offset;
        if (compound)
            child.AddComponent<SphereCollider3DComponent>().m_Offset = offset;
        Rigidbody3DComponent settings;
        settings.m_Type = BodyType3D::Dynamic;
        child.AddComponent<Rigidbody3DComponent>(settings);

        const Ref<JoltBody> body = GetScene().GetPhysicsScene()->GetBody(child);
        ASSERT_TRUE(body);
        const glm::vec3 center(GetScene().ComputeWorldTransform(child) * glm::vec4(offset, 1.0f));
        EXPECT_NEAR(glm::length(body->GetCenterOfMassPosition() - center), 0.0f, 1.0e-4f);
    }
}

TEST_F(ParentedPhysicsPlacementTest, LiveColliderGizmosFollowTheNativeCompoundAfterRotation)
{
    Entity rig = GetScene().CreateEntity("Scaled Collider Rig");
    rig.GetComponent<TransformComponent>().Scale.x = 2.0f;
    Entity child = GetScene().CreateEntity("Compound Body");
    child.SetParent(rig);
    child.AddComponent<BoxCollider3DComponent>().m_Offset.x = 1.0f;
    child.AddComponent<SphereCollider3DComponent>().m_Offset.x = 2.0f;
    child.AddComponent<CapsuleCollider3DComponent>().m_Offset.x = 3.0f;
    Rigidbody3DComponent settings;
    settings.m_Type = BodyType3D::Dynamic;
    child.AddComponent<Rigidbody3DComponent>(settings);
    Ref<JoltBody> body = GetScene().GetPhysicsScene()->GetBody(child);
    ASSERT_TRUE(body);
    body->SetRotation(glm::quat(glm::vec3(0.0f, 0.0f, glm::half_pi<f32>())));

    TArray<PrimitiveColliderGizmo> gizmos;
    body->CollectPrimitiveColliderGizmos(gizmos);
    ASSERT_EQ(gizmos.Num(), 3);
    const glm::vec3 origin = body->GetPosition();
    using Kind = PrimitiveColliderGizmo::Kind;
    for (const auto& gizmo : gizmos)
    {
        const f32 expectedY = gizmo.Type == Kind::Box ? 2.0f : (gizmo.Type == Kind::Sphere ? 4.0f : 6.0f);
        EXPECT_NEAR(glm::length(gizmo.Center - origin - glm::vec3(0.0f, expectedY, 0.0f)), 0.0f, 1.0e-4f);
    }
}

TEST_F(ParentedPhysicsPlacementTest, JoltCapsuleKeepsTheRootTwinRotationWithOneCollapsedAxis)
{
    const glm::vec3 euler{ 0.0f, 0.0f, glm::half_pi<f32>() };
    Entity rig = GetScene().CreateEntity("Capsule Rig");
    rig.GetComponent<TransformComponent>().Translation = { 10.0f, 5.0f, 0.0f };
    rig.GetComponent<TransformComponent>().SetRotationEuler(euler);
    const auto addCapsule = [](Entity entity)
    {
        entity.AddComponent<CapsuleCollider3DComponent>();
        Rigidbody3DComponent settings;
        settings.m_Type = BodyType3D::Dynamic;
        entity.AddComponent<Rigidbody3DComponent>(settings);
    };
    auto* physics = GetScene().GetPhysicsScene();
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        SCOPED_TRACE(axis);
        glm::vec3 scale(1.0f);
        scale[axis] = 0.0f;
        Entity child = GetScene().CreateEntity("Child Capsule");
        child.GetComponent<TransformComponent>().Scale = scale;
        child.GetComponent<TransformComponent>().Translation.z = static_cast<f32>(axis) * 3.0f;
        child.SetParent(rig);
        addCapsule(child);

        Entity twin = GetScene().CreateEntity("Root Capsule");
        auto& transform = twin.GetComponent<TransformComponent>();
        transform.Scale = scale;
        transform.Translation = { 10.0f, 5.0f, static_cast<f32>(axis) * 3.0f };
        transform.SetRotationEuler(euler);
        addCapsule(twin);

        const Ref<JoltBody> childBody = physics->GetBody(child);
        const Ref<JoltBody> twinBody = physics->GetBody(twin);
        ASSERT_TRUE(childBody && twinBody);
        EXPECT_NEAR(glm::length(childBody->GetPosition() - twinBody->GetPosition()), 0.0f, 1.0e-4f);
        EXPECT_NEAR(std::abs(glm::dot(childBody->GetRotation(), twinBody->GetRotation())), 1.0f, 1.0e-4f);
    }
}

TEST_F(ParentedPhysicsPlacementTest, JoltCapsuleKeepsItsSurvivingYAxisUnderACollapsedParent)
{
    Entity rig = GetScene().CreateEntity("Collapsed Rig");
    rig.GetComponent<TransformComponent>().Scale.y = 0.0f;
    Entity capsule = GetScene().CreateEntity("Slanted Capsule");
    capsule.SetParent(rig);
    capsule.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, 0.0f, glm::quarter_pi<f32>() });
    capsule.AddComponent<CapsuleCollider3DComponent>();
    Rigidbody3DComponent settings;
    settings.m_Type = BodyType3D::Dynamic;
    capsule.AddComponent<Rigidbody3DComponent>(settings);

    const auto body = GetScene().GetPhysicsScene()->GetBody(capsule);
    ASSERT_TRUE(body);
    const glm::vec3 axis = body->GetRotation() * glm::vec3(0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(glm::length(axis - glm::vec3(-1.0f, 0.0f, 0.0f)), 0.0f, 1.0e-4f);
}

TEST_F(ParentedPhysicsPlacementTest, CapsuleRejectsMotionInAndOutOfSingularPlaneThenResumes)
{
    Entity parent = GetScene().CreateEntity("Collapsed carrier");
    auto& carrier = parent.GetComponent<TransformComponent>();
    carrier.Scale = { 1.0f, 0.0f, 1.0f };
    Entity capsule = GetScene().CreateEntity("Moving capsule");
    capsule.SetParent(parent);
    capsule.AddComponent<CapsuleCollider3DComponent>();
    Rigidbody3DComponent settings;
    settings.m_Type = BodyType3D::Dynamic;
    settings.m_DisableGravity = true;
    capsule.AddComponent<Rigidbody3DComponent>(settings);
    auto body = GetScene().GetPhysicsScene()->GetBody(capsule);
    ASSERT_TRUE(body);
    ASSERT_TRUE(GetScene().HasSingularParentFrame(capsule, false));

    for (const glm::vec3 native : { glm::vec3(4, 0, 2), glm::vec3(4, -3, 2) })
    {
        body->SetPosition(native);
        RunFrames(1);
        EXPECT_NEAR(glm::distance(body->GetPosition(), native), 0.0f, 1.0e-4f);
        EXPECT_NEAR(glm::length(GetScene().GetWorldPosition(capsule)), 0.0f, 1.0e-4f);
        EXPECT_NEAR(glm::length(capsule.GetComponent<TransformComponent>().Translation), 0.0f, 1.0e-4f);
    }
    carrier.Scale.y = 2.0f;
    EXPECT_FALSE(GetScene().HasSingularParentFrame(capsule, false));
    RunFrames(1);
    EXPECT_NEAR(glm::distance(GetScene().GetWorldPosition(capsule), body->GetPosition()), 0.0f, 1.0e-4f);
    const glm::vec3 saved = capsule.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(saved.y, -1.5f, 1.0e-4f);
    carrier.Scale.y = 0.0f;
    body->SetPosition({ 7, -4, 3 });
    RunFrames(1);
    EXPECT_NEAR(glm::distance(capsule.GetComponent<TransformComponent>().Translation, saved), 0.0f, 1.0e-4f);
    EXPECT_NEAR(glm::distance(GetScene().GetWorldPosition(capsule), glm::vec3(4, 0, 2)), 0.0f, 1.0e-4f);
    carrier.Scale.y = 1.0f;
    RunFrames(1);
    EXPECT_NEAR(glm::distance(GetScene().GetWorldPosition(capsule), body->GetPosition()), 0.0f, 1.0e-4f);
}

TEST_F(ParentedPhysicsPlacementTest, StronglyAnisotropicParentStillConvertsTheRequestedWorldHeading)
{
    auto& parent = m_Rig.GetComponent<TransformComponent>();
    parent.SetRotation(glm::quat(1, 0, 0, 0));
    parent.Scale = { 1.0e-8f, 1.0f, 1.0f };
    const glm::quat desired = glm::angleAxis(glm::quarter_pi<f32>(), glm::vec3(0, 0, 1));
    ASSERT_TRUE(GetScene().TrySetWorldRotation(m_3D[0], desired));
    const glm::vec3 worldX = glm::normalize(glm::vec3(GetScene().ComputeWorldTransform(m_3D[0])[0]));
    EXPECT_NEAR(glm::distance(worldX, desired * glm::vec3(1, 0, 0)), 0.0f, 1.0e-5f);
}

TEST_F(ParentedPhysicsPlacementTest, UnrepresentableFloatRotationDoesNotDiscardNativeTranslation)
{
    auto& parent = m_Rig.GetComponent<TransformComponent>();
    parent.SetRotation(glm::quat(1, 0, 0, 0));
    parent.Scale = { 1.0f, 1.0e-8f, 1.0f };
    auto& local = m_3D[0].GetComponent<TransformComponent>();
    const glm::quat original = local.GetRotation();
    const glm::quat desired = glm::angleAxis(glm::quarter_pi<f32>(), glm::vec3(0, 0, 1));
    EXPECT_FALSE(GetScene().TrySetWorldRotation(m_3D[0], desired));
    EXPECT_FLOAT_EQ(glm::dot(local.GetRotation(), original), 1.0f);
    auto body = GetScene().GetPhysicsScene()->GetBody(m_3D[0]);
    ASSERT_TRUE(body);
    body->SetRotation(desired);
    body->SetPosition({ 6, 5, 4 });
    RunFrames(1);
    EXPECT_NEAR(glm::distance(GetScene().ComputeWorldPosition(m_3D[0]), body->GetPosition()), 0.0f, 1.0e-4f);
    EXPECT_FLOAT_EQ(glm::dot(local.GetRotation(), original), 1.0f);
}

TEST_F(ParentedPhysicsPlacementTest, HeadingValidationUsesTheStoredFloatQuaternion)
{
    auto& parent = m_Rig.GetComponent<TransformComponent>();
    parent.SetRotation(glm::quat(1, 0, 0, 0));
    parent.Scale = { 1, 1.0e-12f, 1 };
    const glm::quat original = m_3D[0].GetComponent<TransformComponent>().GetRotation();
    const auto desired = glm::angleAxis(1.0e-4f, glm::vec3(0, 0, 1));
    if (GetScene().TrySetWorldRotation(m_3D[0], desired))
    {
        const glm::vec3 storedX = glm::normalize(glm::vec3(GetScene().ComputeWorldTransform(m_3D[0])[0]));
        EXPECT_NEAR(glm::distance(storedX, desired * glm::vec3(1, 0, 0)), 0.0f, 2.0e-4f);
    }
    else
        EXPECT_FLOAT_EQ(glm::dot(original, m_3D[0].GetComponent<TransformComponent>().GetRotation()), 1.0f);
}

TEST_F(ParentedPhysicsPlacementTest, SphereWithOneSurvivingScaleAxisKeepsWorldAuthority)
{
    Entity carrier = GetScene().CreateEntity("Full rank carrier");
    Entity sphere = GetScene().CreateEntity("One-axis sphere collider");
    sphere.SetParent(carrier);
    sphere.GetComponent<TransformComponent>().Scale = { 1, 0, 0 };
    sphere.AddComponent<SphereCollider3DComponent>();
    Rigidbody3DComponent settings;
    settings.m_Type = BodyType3D::Dynamic;
    settings.m_DisableGravity = true;
    sphere.AddComponent<Rigidbody3DComponent>(settings);
    auto body = GetScene().GetPhysicsScene()->GetBody(sphere);
    ASSERT_TRUE(body);
    body->SetPosition({ 6, 2, -3 });
    body->SetRotation(glm::angleAxis(0.8f, glm::vec3(0, 1, 0)));
    RunFrames(1);
    EXPECT_NEAR(glm::distance(GetScene().ComputeWorldPosition(sphere), body->GetPosition()), 0.0f, 1.0e-4f);
    EXPECT_NEAR(glm::distance(glm::vec3(GetScene().ComputeWorldTransform(sphere)[0]), body->GetRotation() * glm::vec3(1, 0, 0)), 0.0f, 1.0e-4f);
}

TEST_F(ParentedPhysicsPlacementTest, SmallFullRankParentRetainsFiniteLocalWriteback)
{
    // The reciprocal determinant of this basis overflows float; its inverse axes fit.
    m_Rig.GetComponent<TransformComponent>().Scale = glm::vec3(1.0e-14f);
    ASSERT_FALSE(GetScene().HasSingularParentFrame(m_3D[0], false));
    auto body = GetScene().GetPhysicsScene()->GetBody(m_3D[0]);
    ASSERT_TRUE(body);
    body->SetPosition({ 1, 2, 3 });
    RunFrames(1);
    EXPECT_NEAR(glm::distance(GetScene().ComputeWorldPosition(m_3D[0]), body->GetPosition()), 0.0f, 1.0e-4f);
    const b2Vec2 planar = b2Body_GetPosition(m_2D[0].GetComponent<Rigidbody2DComponent>().RuntimeBody);
    const glm::vec3 world = GetScene().ComputeWorldPosition(m_2D[0]);
    EXPECT_NEAR(world.x, planar.x, 1.0e-4f);
    EXPECT_NEAR(world.y, planar.y, 1.0e-4f);
}

TEST_F(ParentedPhysicsPlacementTest, Box2DWritebackResumesWithAnInvertibleXYParent)
{
    auto& carrier = m_Rig.GetComponent<TransformComponent>();
    carrier.Scale.y = 0.0f;
    const glm::vec3 saved = m_2D[0].GetComponent<TransformComponent>().Translation;
    ASSERT_TRUE(GetScene().HasSingularParentFrame(m_2D[0], true));
    RunFrames(3);
    EXPECT_NEAR(glm::distance(m_2D[0].GetComponent<TransformComponent>().Translation, saved), 0.0f, 1.0e-5f);
    carrier.Scale = { 2.0f, 2.0f, 0.0f };
    EXPECT_FALSE(GetScene().HasSingularParentFrame(m_2D[0], true));
    RunFrames(1);
    const b2Vec2 native = b2Body_GetPosition(m_2D[0].GetComponent<Rigidbody2DComponent>().RuntimeBody);
    const glm::vec3 world = GetScene().GetWorldPosition(m_2D[0]);
    EXPECT_NEAR(world.x, native.x, 1.0e-4f);
    EXPECT_NEAR(world.y, native.y, 1.0e-4f);
}

class PlanarColliderPlacementTest : public FunctionalTest
{
  protected:
    void BuildScene() override {}
};

TEST_F(PlanarColliderPlacementTest, BoxStartingOnCollapsedCarrierGainsItsShapeOnRecovery)
{
    Entity rig = GetScene().CreateEntity("Collapsed rig");
    rig.GetComponent<TransformComponent>().Scale = { 1, 0, 1 };
    Entity child = GetScene().CreateEntity("Box");
    child.SetParent(rig);
    child.AddComponent<BoxCollider2DComponent>();
    child.AddComponent<Rigidbody2DComponent>().Type = Rigidbody2DComponent::BodyType::Dynamic;
    EnablePhysics2D();
    const auto native = child.GetComponent<Rigidbody2DComponent>().RuntimeBody;
    ASSERT_TRUE(b2Body_IsValid(native));
    EXPECT_EQ(b2Body_GetShapeCount(native), 0);
    // A body awaiting its first shape has zero mass. Prescribe motion to
    // exercise native integration without assuming a gravity response.
    b2Body_SetLinearVelocity(native, { 0.0f, -3.0f });
    RunFrames(4);
    EXPECT_LT(b2Body_GetPosition(native).y, 0.0f);
    EXPECT_NEAR(GetScene().ComputeWorldPosition(child).y, 0.0f, 1.0e-5f);
    rig.GetComponent<TransformComponent>().Scale.y = 2;
    RunFrames(2);
    EXPECT_EQ(b2Body_GetShapeCount(native), 1);
    EXPECT_NEAR(GetScene().ComputeWorldPosition(child).y, b2Body_GetPosition(native).y, 1.0e-5f);
}

TEST_F(PlanarColliderPlacementTest, CollinearBoxWaitsAndUsesTheRotatedNativeFrameOnRecovery)
{
    Entity rig = GetScene().CreateEntity("Collapsed XY frame");
    rig.GetComponent<TransformComponent>().Scale = { 0, 2, 1 };
    Entity child = GetScene().CreateEntity("Angled box");
    child.SetParent(rig);
    child.GetComponent<TransformComponent>().SetRotationEuler({ 0, 0, -glm::quarter_pi<f32>() });
    child.AddComponent<BoxCollider2DComponent>().Offset = { 1, 0 };
    child.AddComponent<Rigidbody2DComponent>().Type = Rigidbody2DComponent::BodyType::Dynamic;
    EnablePhysics2D();
    const auto native = child.GetComponent<Rigidbody2DComponent>().RuntimeBody;
    ASSERT_TRUE(b2Body_IsValid(native));
    EXPECT_EQ(b2Body_GetShapeCount(native), 0);
    b2Body_SetTransform(native, { 4, 8 }, b2MakeRot(glm::half_pi<f32>()));
    RunFrames(1);
    EXPECT_EQ(b2Body_GetShapeCount(native), 0);
    rig.GetComponent<TransformComponent>().Scale.x = 1;
    RunFrames(1);
    b2ShapeId shape;
    ASSERT_EQ(b2Body_GetShapes(native, &shape, 1), 1);
    const b2Vec2 centre = b2Body_GetWorldPoint(native, b2Shape_GetPolygon(shape).centroid);
    const glm::vec3 expected(GetScene().ComputeWorldTransform(child) * glm::vec4(1, 0, 0, 1));
    EXPECT_NEAR(centre.x, expected.x, 1.0e-4f);
    EXPECT_NEAR(centre.y, expected.y, 1.0e-4f);
    EXPECT_NEAR(centre.y - b2Body_GetPosition(native).y, 2.0f, 1.0e-4f);
}

TEST_F(PlanarColliderPlacementTest, OffsetBoxRecoversWhenItsCornersBecomeDistinct)
{
    Entity child = GetScene().CreateEntity("Offset box");
    auto& collider = child.AddComponent<BoxCollider2DComponent>();
    collider.Size = { 0.001f, 1.0f };
    collider.Offset.x = 100000.0f;
    child.AddComponent<Rigidbody2DComponent>().Type = Rigidbody2DComponent::BodyType::Dynamic;
    EnablePhysics2D();
    const auto native = child.GetComponent<Rigidbody2DComponent>().RuntimeBody;
    ASSERT_TRUE(b2Body_IsValid(native));
    EXPECT_EQ(b2Body_GetShapeCount(native), 0);
    collider.Offset.x = 1.0f;
    RunFrames(1);
    EXPECT_EQ(b2Body_GetShapeCount(native), 1);
}

TEST_F(PlanarColliderPlacementTest, BoxAndCircleCentersFollowScaleAndMirrorWithZeroZ)
{
    Entity rig = GetScene().CreateEntity("Planar Mirrored Rig");
    auto& rigPose = rig.GetComponent<TransformComponent>();
    rigPose.Translation = { 12.0f, 2.0f, 0.0f };
    rigPose.Scale = { -2.0f, 1.0f, 0.0f };
    rigPose.SetRotationEuler({ 0.0f, 0.0f, glm::half_pi<f32>() });
    const glm::vec2 offset(1.0f, 0.5f);
    std::array<Entity, 3> bodies;
    for (sizet i = 0; i < bodies.size(); ++i)
    {
        Entity entity = GetScene().CreateEntity("Planar Collider");
        auto& local = entity.GetComponent<TransformComponent>();
        local.Translation = { static_cast<f32>(i) * 5.0f, 0.0f, 0.0f };
        local.Scale.z = 0.0f;
        if (i < 2)
            entity.SetParent(rig);
        else
            local.Scale = { -2.0f, 1.0f, 0.0f };
        entity.AddComponent<Rigidbody2DComponent>();
        if (i == 1)
            entity.AddComponent<CircleCollider2DComponent>().Offset = offset;
        else
            entity.AddComponent<BoxCollider2DComponent>().Offset = offset;
        bodies[i] = entity;
    }
    EnablePhysics2D();

    for (sizet i = 0; i < bodies.size(); ++i)
    {
        Entity entity = bodies[i];
        const b2BodyId body = entity.GetComponent<Rigidbody2DComponent>().RuntimeBody;
        ASSERT_TRUE(b2Body_IsValid(body));
        b2ShapeId shape;
        ASSERT_EQ(b2Body_GetShapes(body, &shape, 1), 1);
        const b2Vec2 localCenter = i == 1 ? b2Shape_GetCircle(shape).center : b2Shape_GetPolygon(shape).centroid;
        const auto center = b2Body_GetWorldPoint(body, localCenter);
        const glm::vec2 expected(GetScene().ComputeWorldTransform(entity) * glm::vec4(offset, 0.0f, 1.0f));
        EXPECT_NEAR(center.x, expected.x, 1.0e-4f);
        EXPECT_NEAR(center.y, expected.y, 1.0e-4f);
        EXPECT_NEAR(b2Rot_GetAngle(b2Body_GetRotation(body)), i < 2 ? -glm::half_pi<f32>() : 0.0f, 1.0e-4f);
    }

    rigPose.Translation.x += 20.0f;
    RunFrames(1);
    const b2BodyId body = bodies[0].GetComponent<Rigidbody2DComponent>().RuntimeBody;
    EXPECT_NEAR(GetScene().GetWorldPosition(bodies[0]).x, b2Body_GetPosition(body).x, 1.0e-4f);
}

TEST_F(PlanarColliderPlacementTest, ProjectedBox2DRotationKeepsTheAuthoredPitchWhenPossible)
{
    Entity rig = GetScene().CreateEntity("Pitched Rig");
    rig.GetComponent<TransformComponent>().SetRotationEuler({ glm::radians(60.0f), 0.0f, 0.0f });
    Entity entity = GetScene().CreateEntity("Pitched Collider");
    auto& transform = entity.GetComponent<TransformComponent>();
    transform.SetRotationEuler({ 0.0f, glm::radians(30.0f), 0.0f });
    entity.SetParent(rig);
    entity.AddComponent<Rigidbody2DComponent>();
    entity.AddComponent<BoxCollider2DComponent>();
    EnablePhysics2D();
    const b2BodyId body = entity.GetComponent<Rigidbody2DComponent>().RuntimeBody;
    ASSERT_TRUE(b2Body_IsValid(body));
    const b2Rot startingRotation = b2Body_GetRotation(body);
    const f32 startingAngle = std::atan2(startingRotation.s, startingRotation.c);
    // Use Box2D's deterministic rotation representation for the expected
    // composed angle. Its approximation is visible at this tolerance.
    const b2Rot expectedRotation = b2MakeRot(std::atan(0.5f));
    EXPECT_NEAR(startingAngle, std::atan2(expectedRotation.s, expectedRotation.c), 1.0e-4f);

    RunFrames(1);
    const glm::vec3 initialX(GetScene().GetWorldTransform(entity)[0]);
    EXPECT_NEAR(std::atan2(initialX.y, initialX.x), startingAngle, 1.0e-4f);

    const f32 movedAngle = 0.65f;
    b2Body_SetTransform(body, b2Body_GetPosition(body), b2MakeRot(movedAngle));
    RunFrames(1);
    const b2Rot bodyRotation = b2Body_GetRotation(body);
    const glm::vec3 movedX(GetScene().GetWorldTransform(entity)[0]);
    EXPECT_NEAR(std::atan2(movedX.y, movedX.x), std::atan2(bodyRotation.s, bodyRotation.c), 1.0e-4f);
    EXPECT_NEAR(transform.GetRotationEuler().y, glm::radians(30.0f), 1.0e-4f);
}
