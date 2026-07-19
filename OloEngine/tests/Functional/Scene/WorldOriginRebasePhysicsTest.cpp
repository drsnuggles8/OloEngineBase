#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// WorldOriginRebasePhysicsTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::RebaseOrigin (floating-origin, issue #429) × JoltScene physics.
//   A rebase shifts every stored world position — ECS transforms AND live Jolt
//   bodies — by the same delta in one atomic game-thread pass. The physics
//   simulation must not notice: velocities are preserved (a body in flight
//   keeps moving), and static geometry shifts with the dynamic bodies (a body
//   resting on a floor stays resting instead of falling through a floor that
//   moved out from under it).
//
// This is the "passes math tests but behaves broken" failure mode CLAUDE.md
// warns about: the transform-only unit test (WorldOriginRebaseTest) proves the
// arithmetic; only driving real Jolt across a rebase proves the physics body
// shift is velocity/COM-correct and that the static floor moved too.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h" // GetPhysicsScene()->HasWorldAnchoredConstraints()

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

class WorldOriginRebasePhysicsTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // A projectile with a known horizontal velocity, gravity + drag off, so
        // its motion is a clean straight line we can predict across a rebase.
        m_Body = GetScene().CreateEntity("Projectile");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 50.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.3f;
        m_Body.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_DisableGravity = true;
        body.m_LinearDrag = 0.0f;
        body.m_InitialLinearVelocity = { 10.0f, 0.0f, 0.0f }; // 10 m/s along +x
        m_Body.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();
    }

    Entity m_Body;
};

TEST_F(WorldOriginRebasePhysicsTest, RebaseShiftsBodyAndPreservesVelocity)
{
    // Fly for 0.5 s: x ~= 5.
    TickFor(0.5f);
    const f32 xBeforeRebase = m_Body.GetComponent<TransformComponent>().Translation.x;
    EXPECT_NEAR(xBeforeRebase, 5.0f, 0.3f) << "projectile didn't fly as expected before rebase";

    // Rebase the whole world 1024 m along -x (as the origin trigger would once
    // the camera passed the threshold). The body should teleport with it.
    const glm::vec3 shift{ -1024.0f, 0.0f, 0.0f };
    GetScene().RebaseOrigin(shift);

    // Snapshot BY VALUE — TransformComponent.Translation is live and would keep
    // changing as the body flies on below, aliasing xFinal.
    const glm::vec3 afterShift = m_Body.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(afterShift.x, xBeforeRebase + shift.x, 0.05f)
        << "body transform did not shift with the rebase";
    EXPECT_TRUE(std::isfinite(afterShift.x) && std::isfinite(afterShift.y) && std::isfinite(afterShift.z));

    // Absolute (authored-frame) position is recovered by the offset.
    EXPECT_NEAR(GetScene().RebasedToAbsolute(afterShift).x, xBeforeRebase, 0.05f);
    EXPECT_NEAR(GetScene().GetWorldOrigin().x, 1024.0f, 1e-3f);

    // Velocity was preserved by the shift (DontActivate SetPosition): another
    // 0.5 s advances x by ~5 more from the shifted position, not from rest.
    TickFor(0.5f);
    const f32 xFinal = m_Body.GetComponent<TransformComponent>().Translation.x;
    EXPECT_NEAR(xFinal, afterShift.x + 5.0f, 0.3f)
        << "body did not keep moving after the rebase — velocity was lost in the shift";

    // And the absolute trajectory is a continuous ~10 m over the full second.
    EXPECT_NEAR(GetScene().RebasedToAbsolute(m_Body.GetComponent<TransformComponent>().Translation).x,
                10.0f, 0.5f);
}

// ---------------------------------------------------------------------------
// Second scenario: a body settled on a STATIC floor must stay settled after a
// rebase — proving the static floor body shifted too (a bug that only shifted
// dynamic bodies would drop the resting body through the moved-away floor).
// ---------------------------------------------------------------------------
class WorldOriginRebaseFloorTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Static floor at y = 0.
        m_Floor = GetScene().CreateEntity("Floor");
        m_Floor.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        m_Floor.AddComponent<BoxCollider3DComponent>(floorCol);
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        m_Floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // Dynamic box dropped just above the floor.
        m_Box = GetScene().CreateEntity("Box");
        m_Box.GetComponent<TransformComponent>().Translation = { 0.0f, 2.0f, 0.0f };
        BoxCollider3DComponent boxCol;
        boxCol.m_HalfExtents = { 0.5f, 0.5f, 0.5f };
        m_Box.AddComponent<BoxCollider3DComponent>(boxCol);
        Rigidbody3DComponent boxBody;
        boxBody.m_Type = BodyType3D::Dynamic;
        boxBody.m_Mass = 1.0f;
        m_Box.AddComponent<Rigidbody3DComponent>(boxBody);

        EnablePhysics3D();
    }

    Entity m_Floor;
    Entity m_Box;
};

TEST_F(WorldOriginRebaseFloorTest, RestingBodyStaysOnFloorAcrossRebase)
{
    // Let the box settle on the floor (box centre rests near y = 1.0).
    TickFor(2.0f);
    const f32 restY = m_Box.GetComponent<TransformComponent>().Translation.y;
    EXPECT_NEAR(restY, 1.0f, 0.2f) << "box did not settle on the floor before the rebase";

    // Rebase far horizontally. Floor and box both shift; the box must stay on
    // the (also-shifted) floor.
    GetScene().RebaseOrigin(glm::vec3{ 3072.0f, 0.0f, -2048.0f });

    // Simulate more; the box must not fall through a floor that stayed behind.
    TickFor(1.0f);
    const auto& p = m_Box.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(p.y, restY, 0.2f)
        << "box fell (or jumped) after the rebase — the static floor body did not shift with it";
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
}

// ---------------------------------------------------------------------------
// Third scenario: a joint anchored to the fixed world (m_ConnectedEntity == 0
// -> Body::sFixedToWorld) holds an ABSOLUTE world anchor. As of issue #613
// JoltScene::ShiftWorldAnchoredConstraints translates that anchor with the body
// during ShiftOrigin (via NotifyShapeChanged on the sFixedToWorld side), so the
// auto-trigger now APPLIES the rebase instead of deferring it — and the body
// stays fixed relative to its (shifted) anchor rather than being yanked. This is
// the direct regression guard for the defer-removal: without the anchor shift
// the body would be teleported by the rebase while its world anchor stayed
// behind, and the stiff Fixed joint would violently pull it back over the next
// ticks.
// ---------------------------------------------------------------------------
class WorldOriginRebaseWorldJointTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Body = GetScene().CreateEntity("AnchoredBody");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.3f;
        m_Body.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        m_Body.AddComponent<Rigidbody3DComponent>(body);

        // A Fixed joint with no connected entity -> anchored to the fixed world
        // body (an absolute-world-space anchor).
        PhysicsJoint3DComponent joint;
        joint.m_Type = JointType3D::Fixed;
        joint.m_ConnectedEntity = 0; // world anchor
        m_Body.AddComponent<PhysicsJoint3DComponent>(joint);

        EnablePhysics3D();
    }

    Entity m_Body;
};

TEST_F(WorldOriginRebaseWorldJointTest, AutoRebaseAppliesAndShiftsWorldAnchorWithBody)
{
    // Verify the scene really built the world-anchored constraint (the predicate
    // is retained for diagnostics even though it no longer gates the rebase).
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_TRUE(GetScene().GetPhysicsScene()->HasWorldAnchoredConstraints())
        << "expected a world-anchored (fixed-to-world) constraint to be present";

    // Let the joint settle so the body sits exactly on its world anchor.
    TickFor(0.5f);
    const glm::vec3 before = m_Body.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(before.y, 5.0f, 0.1f) << "fixed-to-world joint did not hold the body at its anchor";

    // Enable rebasing with a low threshold so a far reference triggers a rebase.
    WorldOriginSettings s;
    s.Enabled = true;
    s.RebaseThreshold = 1024.0f;
    s.SnapGridSize = 1024.0f;
    GetScene().SetWorldOriginSettings(s);

    // The reference is far past the threshold: the rebase now APPLIES (non-zero
    // shift) and the anchor is translated with the body — no defer.
    const glm::vec3 shift = GetScene().MaybeRebaseOrigin(glm::vec3(6000.0f, 0.0f, 0.0f));
    ASSERT_GT(glm::dot(shift, shift), 0.0f) << "rebase should have applied, not deferred";

    // The body teleported with the rebase (snapshot by value — it is live).
    const glm::vec3 afterShift = m_Body.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(afterShift.x, before.x + shift.x, 0.05f) << "body did not shift with the rebase";
    EXPECT_NEAR(afterShift.z, before.z + shift.z, 0.05f);
    EXPECT_TRUE(std::isfinite(afterShift.x) && std::isfinite(afterShift.y) && std::isfinite(afterShift.z));
    EXPECT_FLOAT_EQ(GetScene().GetWorldOrigin().x, -shift.x);

    // The crux: the world anchor moved WITH the body. Simulate more — the stiff
    // Fixed joint must hold the body at its shifted anchor, not yank it back
    // toward the old (un-shifted) world point. A missed anchor shift would show
    // as a large displacement here as the joint snaps the body ~|shift| away.
    TickFor(1.0f);
    const glm::vec3 settled = m_Body.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(settled.x, afterShift.x, 0.25f)
        << "body was yanked after the rebase — the world anchor did not shift with it";
    EXPECT_NEAR(settled.y, afterShift.y, 0.25f);
    EXPECT_NEAR(settled.z, afterShift.z, 0.25f);
    EXPECT_TRUE(std::isfinite(settled.x) && std::isfinite(settled.y) && std::isfinite(settled.z));

    // And the absolute (authored-frame) position is recovered: the body is still
    // at its original world anchor in absolute space.
    const glm::vec3 absPos = GetScene().RebasedToAbsolute(settled);
    EXPECT_NEAR(absPos.x, before.x, 0.3f);
    EXPECT_NEAR(absPos.y, before.y, 0.3f);
    EXPECT_NEAR(absPos.z, before.z, 0.3f);
}

// ---------------------------------------------------------------------------
// Fourth scenario: cloth soft body. A rebase must translate every soft-body
// vertex by exactly the shift (moving the body's COM carries the whole COM-
// relative particle cloud), AND shift the CPU render cache in lockstep so a
// frame drawn between the rebase and the next physics sync shows no pop
// (issue #613). Then it keeps simulating without NaN.
// ---------------------------------------------------------------------------
class WorldOriginRebaseClothTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Cloth = GetScene().CreateEntity("Cloth");
        m_Cloth.GetComponent<TransformComponent>().Translation = { 0.0f, 8.0f, 0.0f };
        auto& cloth = m_Cloth.AddComponent<ClothComponent>();
        cloth.m_Columns = 10;
        cloth.m_Rows = 10;
        cloth.m_Width = 3.0f;
        cloth.m_Height = 3.0f;
        cloth.m_Mass = 1.0f;
        cloth.m_Attachment = ClothAttachment::TopEdge;
        cloth.m_Enabled = true;

        EnablePhysics3D();
    }

    Entity m_Cloth;
};

TEST_F(WorldOriginRebaseClothTest, ClothVerticesShiftRigidlyWithRebase)
{
    const UUID clothID = m_Cloth.GetUUID();

    // Drape a little so the vertices spread into a non-trivial shape (not a flat
    // grid), making a rigid-translation check meaningful.
    TickFor(0.5f);

    const std::vector<glm::vec3>* beforePtr = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(beforePtr, nullptr) << "cloth soft body has no readback";
    const std::vector<glm::vec3> before = *beforePtr; // copy — it is live
    ASSERT_GT(before.size(), 0u);

    const glm::vec3 shift{ -4096.0f, 0.0f, 2048.0f };
    GetScene().RebaseOrigin(shift);

    const std::vector<glm::vec3>* afterPtr = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(afterPtr, nullptr);
    const std::vector<glm::vec3>& after = *afterPtr;
    ASSERT_EQ(after.size(), before.size());

    // Every vertex moved by EXACTLY the shift — the whole particle cloud
    // translated rigidly (internal vertex-to-vertex offsets preserved). This is
    // read straight from the render cache immediately after the rebase (no tick
    // in between), so it also proves the CPU cache was shifted in lockstep.
    for (sizet i = 0; i < before.size(); ++i)
    {
        EXPECT_NEAR(after[i].x, before[i].x + shift.x, 0.02f) << "cloth vertex " << i << " x diverged";
        EXPECT_NEAR(after[i].y, before[i].y + shift.y, 0.02f) << "cloth vertex " << i << " y diverged";
        EXPECT_NEAR(after[i].z, before[i].z + shift.z, 0.02f) << "cloth vertex " << i << " z diverged";
    }

    // It keeps simulating sanely after the rebase (no NaN, pinned edge still up).
    TickFor(0.5f);
    const std::vector<glm::vec3>* settledPtr = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(settledPtr, nullptr);
    f32 maxY = -std::numeric_limits<f32>::max();
    for (const glm::vec3& p : *settledPtr)
    {
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        maxY = std::max(maxY, p.y);
    }
    // Pinned top edge still hangs near its (shifted) spawn height (8 + shift.y = 8).
    EXPECT_GT(maxY, 8.0f - 1.0f) << "pinned cloth edge fell after the rebase";
}

// ---------------------------------------------------------------------------
// Fifth scenario: a pulley — the one world-anchored constraint whose fixed
// pivots have no runtime setter, so RebaseOrigin shifts the authored pivots and
// rebuilds it from the shifted transforms. Verify the pivots shifted, the bodies
// teleported with the world, and the rebuilt pulley keeps them sane (#613).
// ---------------------------------------------------------------------------
class WorldOriginRebasePulleyTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto makeBody = [this](const char* name, const glm::vec3& pos) -> Entity
        {
            Entity e = GetScene().CreateEntity(name);
            e.GetComponent<TransformComponent>().Translation = pos;
            SphereCollider3DComponent col;
            col.m_Radius = 0.3f;
            e.AddComponent<SphereCollider3DComponent>(col);
            Rigidbody3DComponent body;
            body.m_Type = BodyType3D::Dynamic;
            body.m_Mass = 1.0f;
            e.AddComponent<Rigidbody3DComponent>(body);
            return e;
        };
        m_BodyA = makeBody("PulleyA", { -2.0f, 5.0f, 0.0f });
        m_BodyB = makeBody("PulleyB", { 2.0f, 5.0f, 0.0f });

        PhysicsJoint3DComponent joint;
        joint.m_Type = JointType3D::Pulley;
        joint.m_ConnectedEntity = m_BodyB.GetUUID();
        joint.m_PulleyFixedPointA = { -2.0f, 10.0f, 0.0f };
        joint.m_PulleyFixedPointB = { 2.0f, 10.0f, 0.0f };
        joint.m_PulleyMinLength = 0.0f;
        joint.m_PulleyMaxLength = -1.0f; // auto from current total length
        m_BodyA.AddComponent<PhysicsJoint3DComponent>(joint);

        EnablePhysics3D();
    }

    Entity m_BodyA;
    Entity m_BodyB;
};

TEST_F(WorldOriginRebasePulleyTest, RebaseShiftsPulleyPivotsAndKeepsBodiesSane)
{
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_TRUE(GetScene().GetPhysicsScene()->HasWorldAnchoredConstraints())
        << "a pulley is world-anchored (two absolute fixed pivots)";

    TickFor(0.5f);
    const glm::vec3 aBefore = m_BodyA.GetComponent<TransformComponent>().Translation;
    const glm::vec3 bBefore = m_BodyB.GetComponent<TransformComponent>().Translation;
    const glm::vec3 pivotABefore = m_BodyA.GetComponent<PhysicsJoint3DComponent>().m_PulleyFixedPointA;
    const glm::vec3 pivotBBefore = m_BodyA.GetComponent<PhysicsJoint3DComponent>().m_PulleyFixedPointB;

    const glm::vec3 shift{ 2048.0f, 0.0f, -3072.0f };
    GetScene().RebaseOrigin(shift);

    // The authored fixed pivots were shifted so the rebuilt pulley pins to the
    // same relative geometry (a pulley's pivots have no runtime setter — this is
    // the destroy+rebuild-from-shifted-settings path).
    const auto& jointAfter = m_BodyA.GetComponent<PhysicsJoint3DComponent>();
    EXPECT_NEAR(jointAfter.m_PulleyFixedPointA.x, pivotABefore.x + shift.x, 1e-3f);
    EXPECT_NEAR(jointAfter.m_PulleyFixedPointA.z, pivotABefore.z + shift.z, 1e-3f);
    EXPECT_NEAR(jointAfter.m_PulleyFixedPointB.x, pivotBBefore.x + shift.x, 1e-3f);
    EXPECT_NEAR(jointAfter.m_PulleyFixedPointB.z, pivotBBefore.z + shift.z, 1e-3f);

    // Bodies teleported with the rebase.
    EXPECT_NEAR(m_BodyA.GetComponent<TransformComponent>().Translation.x, aBefore.x + shift.x, 0.1f);
    EXPECT_NEAR(m_BodyB.GetComponent<TransformComponent>().Translation.x, bBefore.x + shift.x, 0.1f);

    // The constraint was rebuilt and still exists (still world-anchored).
    EXPECT_TRUE(GetScene().GetPhysicsScene()->HasWorldAnchoredConstraints())
        << "pulley constraint should survive the rebuild";

    // Simulate more: the pulley holds the bodies in a finite, sane configuration
    // relative to their shifted pivots — no explosion from a stretched rope.
    TickFor(1.0f);
    const glm::vec3 aSettled = m_BodyA.GetComponent<TransformComponent>().Translation;
    const glm::vec3 bSettled = m_BodyB.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(aSettled.x) && std::isfinite(aSettled.y) && std::isfinite(aSettled.z));
    EXPECT_TRUE(std::isfinite(bSettled.x) && std::isfinite(bSettled.y) && std::isfinite(bSettled.z));
    EXPECT_LT(glm::length(aSettled - jointAfter.m_PulleyFixedPointA), 20.0f)
        << "pulley body A was flung far from its pivot after the rebase";
    EXPECT_LT(glm::length(bSettled - jointAfter.m_PulleyFixedPointB), 20.0f)
        << "pulley body B was flung far from its pivot after the rebase";
}

// ---------------------------------------------------------------------------
// Sixth scenario: a long traversal firing MANY sequential auto-rebases (the
// 50 km² acceptance bar in miniature). A fast, drag/gravity-free traveler flies
// straight while the auto-trigger rebases it every time it drifts past the
// threshold. The absolute-space trajectory must stay continuous and match the
// un-rebased prediction across every rebase, while the STORED (rebased)
// coordinate stays small — the whole point of floating origin (#613 / #429).
// ---------------------------------------------------------------------------
class WorldOriginRebaseTraversalTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Body = GetScene().CreateEntity("Traveler");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 50.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.3f;
        m_Body.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_DisableGravity = true;
        body.m_LinearDrag = 0.0f;
        body.m_InitialLinearVelocity = { kSpeed, 0.0f, 0.0f };
        m_Body.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();
    }

    static constexpr f32 kSpeed = 500.0f; // jet-like — many rebases in little sim time

    Entity m_Body;
};

TEST_F(WorldOriginRebaseTraversalTest, ManySequentialRebasesPreserveAbsoluteTrajectory)
{
    WorldOriginSettings s;
    s.Enabled = true;
    s.RebaseThreshold = 1024.0f;
    s.SnapGridSize = 1024.0f;
    GetScene().SetWorldOriginSettings(s);

    // Reach steady-state velocity before measuring (skip the body-creation /
    // initial-velocity-application transient).
    TickFor(0.5f);

    // Continuity is the real property under test: absolute-space position advances
    // by the SAME ~v·dt every step, whether or not a rebase fired that step. A
    // missed shift compensation would show as a ~|shift| (≈1024 m) jump in one
    // step — impossible to hide. This holds independent of any fixed startup lag,
    // unlike an absolute v·elapsed prediction.
    const f32 kStep = 0.1f;
    const f32 kExpectedAdvance = kSpeed * kStep; // ≈ 50 m per step
    f32 prevAbsX = GetScene().RebasedToAbsolute(m_Body.GetComponent<TransformComponent>().Translation).x;
    i32 rebaseCount = 0;

    // 200 * 0.1 s = 20 s at 500 m/s ≈ 10 km of travel → ~10 rebases.
    for (i32 step = 0; step < 200; ++step)
    {
        TickFor(kStep);

        // The traveler is its own reference (a first-person camera): when its
        // stored position drifts past the threshold, the auto-trigger snaps the
        // whole world (including the traveler) back toward the origin.
        const glm::vec3 shift = GetScene().MaybeRebaseOrigin(m_Body.GetComponent<TransformComponent>().Translation);
        const bool rebasedThisStep = glm::dot(shift, shift) > 0.0f;
        if (rebasedThisStep)
            ++rebaseCount;

        const glm::vec3 absPos = GetScene().RebasedToAbsolute(m_Body.GetComponent<TransformComponent>().Translation);
        const f32 advance = absPos.x - prevAbsX;
        prevAbsX = absPos.x;

        // The step advance stays smooth across every rebase boundary (band = 15%
        // of the expected advance + 1 m; a ~1024 m rebase jump is nowhere near it).
        EXPECT_NEAR(advance, kExpectedAdvance, kExpectedAdvance * 0.15f + 1.0f)
            << "absolute trajectory jumped at step " << step
            << " (rebased this step: " << rebasedThisStep << ") — the rebase was not continuity-preserving";
        EXPECT_TRUE(std::isfinite(absPos.x) && std::isfinite(absPos.y) && std::isfinite(absPos.z));

        // The STORED coordinate never grows large — floating origin keeps it near
        // the rebased origin (well under 2× the threshold).
        EXPECT_LT(std::abs(m_Body.GetComponent<TransformComponent>().Translation.x), 2100.0f)
            << "stored coordinate grew unbounded — rebasing is not keeping it small";
    }

    EXPECT_GT(rebaseCount, 5) << "expected several rebases across a ~10 km traversal";
}
