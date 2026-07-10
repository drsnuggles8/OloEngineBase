#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// WorldOriginRebaseTest — unit test (headless, no GL, no physics).
//
// Pins the floating-origin / origin-rebasing math added for issue #429:
// Scene::RebaseOrigin, Scene::MaybeRebaseOrigin, and the rebased<->absolute
// coordinate helpers. Camera-relative rendering (the already-shipped
// two-thirds of #429) keeps GPU precision high near the camera but never
// touches stored ECS state; rebasing keeps the STORED f32 world coordinates
// small so physics/gameplay/spatial-query precision stays high too.
//
// The contract under test (no physics, no renderer — pure transform math):
//   1. RebaseOrigin shifts only ROOT entity local translations, so the whole
//      hierarchy translates uniformly — a child's WORLD position moves by
//      exactly the shift while its LOCAL translation is untouched.
//   2. The world-origin accumulator keeps `absolute == rebased + worldOrigin`
//      invariant, and the Rebased<->Absolute helpers round-trip.
//   3. A zero / non-finite shift is a no-op (never corrupts the scene).
//   4. MaybeRebaseOrigin: disabled -> no-op; within threshold -> no-op; past
//      threshold -> snaps the reference back near origin; the built-in
//      hysteresis means an immediate re-check does nothing (no thrash).
//   5. SanitizeWorldOriginSettings enforces the trigger-vs-post-rebase-distance
//      invariant.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/WorldOriginSettings.h"

#include <glm/glm.hpp>

#include <cmath>

using namespace OloEngine;

namespace
{
    glm::vec3 WorldTranslation(Scene& scene, Entity e)
    {
        const glm::mat4 world = scene.GetWorldTransform(static_cast<entt::entity>(e));
        return glm::vec3(world[3]);
    }

    void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, f32 tol = 1e-3f)
    {
        EXPECT_NEAR(actual.x, expected.x, tol);
        EXPECT_NEAR(actual.y, expected.y, tol);
        EXPECT_NEAR(actual.z, expected.z, tol);
    }

    // "Is this shift exactly zero" — the rebase uses squared-length > 0 as its
    // non-zero test, so mirror that here rather than a float ==.
    bool IsZeroVec3(const glm::vec3& v)
    {
        return !(glm::dot(v, v) > 0.0f);
    }
} // namespace

TEST(WorldOriginRebaseTest, RootTranslationShiftsAndOffsetAccumulates)
{
    Ref<Scene> scene = Ref<Scene>::Create();
    Entity e = scene->CreateEntity("Root");
    e.GetComponent<TransformComponent>().Translation = { 5000.0f, 10.0f, -3000.0f };

    EXPECT_TRUE(IsZeroVec3(scene->GetWorldOrigin()));

    const glm::vec3 shift{ -4096.0f, 0.0f, 2048.0f };
    scene->RebaseOrigin(shift);

    const glm::vec3 rebased = e.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(rebased.x, 904.0f, 1e-3f);
    EXPECT_NEAR(rebased.y, 10.0f, 1e-3f);
    EXPECT_NEAR(rebased.z, -952.0f, 1e-3f);

    // absolute = rebased + worldOrigin must recover the authored coordinate.
    EXPECT_NEAR(scene->GetWorldOrigin().x, 4096.0f, 1e-3f);
    EXPECT_NEAR(scene->GetWorldOrigin().y, 0.0f, 1e-3f);
    EXPECT_NEAR(scene->GetWorldOrigin().z, -2048.0f, 1e-3f);

    const glm::vec3 recovered = scene->RebasedToAbsolute(rebased);
    EXPECT_NEAR(recovered.x, 5000.0f, 1e-3f);
    EXPECT_NEAR(recovered.y, 10.0f, 1e-3f);
    EXPECT_NEAR(recovered.z, -3000.0f, 1e-3f);

    // Round-trip both directions.
    const glm::vec3 abs{ 123.0f, -45.0f, 678.0f };
    const glm::vec3 back = scene->RebasedToAbsolute(scene->AbsoluteToRebased(abs));
    EXPECT_NEAR(back.x, abs.x, 1e-3f);
    EXPECT_NEAR(back.y, abs.y, 1e-3f);
    EXPECT_NEAR(back.z, abs.z, 1e-3f);
}

TEST(WorldOriginRebaseTest, ChildWorldMovesButLocalUnchanged)
{
    Ref<Scene> scene = Ref<Scene>::Create();
    Entity parent = scene->CreateEntity("Parent");
    parent.GetComponent<TransformComponent>().Translation = { 8000.0f, 0.0f, 0.0f };

    Entity child = scene->CreateEntity("Child");
    child.GetComponent<TransformComponent>().Translation = { 3.0f, 0.0f, 0.0f }; // local offset
    child.SetParent(parent);

    scene->PropagateWorldTransforms();
    const glm::vec3 childWorldBefore = WorldTranslation(*scene, child);
    EXPECT_NEAR(childWorldBefore.x, 8003.0f, 1e-3f);

    const glm::vec3 shift{ -8192.0f, 0.0f, 0.0f };
    scene->RebaseOrigin(shift);

    // Only the ROOT (parent) local translation shifted.
    EXPECT_NEAR(parent.GetComponent<TransformComponent>().Translation.x, -192.0f, 1e-3f);
    // The child's LOCAL translation is parent-relative — untouched.
    EXPECT_NEAR(child.GetComponent<TransformComponent>().Translation.x, 3.0f, 1e-3f);

    // The child's WORLD position moved by exactly the shift (no double-shift).
    const glm::vec3 childWorldAfter = WorldTranslation(*scene, child);
    EXPECT_NEAR(childWorldAfter.x, childWorldBefore.x + shift.x, 1e-3f);
    EXPECT_NEAR(childWorldAfter.x, -189.0f, 1e-3f);

    // And its absolute world position is recoverable.
    EXPECT_NEAR(scene->RebasedToAbsolute(childWorldAfter).x, childWorldBefore.x, 1e-3f);
}

TEST(WorldOriginRebaseTest, ZeroAndNonFiniteShiftAreNoOps)
{
    Ref<Scene> scene = Ref<Scene>::Create();
    Entity e = scene->CreateEntity("Root");
    e.GetComponent<TransformComponent>().Translation = { 100.0f, 200.0f, 300.0f };

    scene->RebaseOrigin(glm::vec3(0.0f));
    ExpectVec3Near(e.GetComponent<TransformComponent>().Translation, glm::vec3(100.0f, 200.0f, 300.0f));
    EXPECT_TRUE(IsZeroVec3(scene->GetWorldOrigin()));

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    scene->RebaseOrigin(glm::vec3(nan, 0.0f, 0.0f));
    // Position and offset must be untouched (never corrupt the whole scene).
    EXPECT_TRUE(std::isfinite(e.GetComponent<TransformComponent>().Translation.x));
    ExpectVec3Near(e.GetComponent<TransformComponent>().Translation, glm::vec3(100.0f, 200.0f, 300.0f));
    EXPECT_TRUE(IsZeroVec3(scene->GetWorldOrigin()));
}

TEST(WorldOriginRebaseTest, MaybeRebaseRespectsEnableAndThreshold)
{
    Ref<Scene> scene = Ref<Scene>::Create();
    WorldOriginSettings s;
    s.Enabled = false;
    s.RebaseThreshold = 2048.0f;
    s.SnapGridSize = 1024.0f;
    scene->SetWorldOriginSettings(s);

    // Disabled -> never rebases even when far.
    EXPECT_TRUE(IsZeroVec3(scene->MaybeRebaseOrigin(glm::vec3(1.0e6f, 0.0f, 0.0f))));
    EXPECT_TRUE(IsZeroVec3(scene->GetWorldOrigin()));

    s.Enabled = true;
    scene->SetWorldOriginSettings(s);

    // Within threshold -> no-op.
    EXPECT_TRUE(IsZeroVec3(scene->MaybeRebaseOrigin(glm::vec3(1000.0f, 0.0f, 0.0f))));
    EXPECT_TRUE(IsZeroVec3(scene->GetWorldOrigin()));
}

TEST(WorldOriginRebaseTest, MaybeRebasePastThresholdSnapsReferenceBackAndHasHysteresis)
{
    Ref<Scene> scene = Ref<Scene>::Create();
    WorldOriginSettings s;
    s.Enabled = true;
    s.RebaseThreshold = 2048.0f;
    s.SnapGridSize = 1024.0f;
    scene->SetWorldOriginSettings(s);

    // A camera-proxy root entity carries the reference position.
    Entity cam = scene->CreateEntity("Camera");
    const glm::vec3 farPos{ 5000.0f, 0.0f, -5000.0f };
    cam.GetComponent<TransformComponent>().Translation = farPos;

    const glm::vec3 shift = scene->MaybeRebaseOrigin(farPos);
    ASSERT_FALSE(IsZeroVec3(shift));

    // After the rebase the reference sits within +/- grid/2 of the new origin,
    // well inside the threshold (the hysteresis gap that prevents thrash).
    const glm::vec3 rebasedRef = farPos + shift;
    EXPECT_LE(std::abs(rebasedRef.x), s.SnapGridSize * 0.5f + 1e-3f);
    EXPECT_LE(std::abs(rebasedRef.z), s.SnapGridSize * 0.5f + 1e-3f);
    EXPECT_LT(glm::length(rebasedRef), s.RebaseThreshold);

    // The moved entity's translation matches the snapped reference, and its
    // absolute position is preserved.
    const glm::vec3 camAfter = cam.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(camAfter.x, rebasedRef.x, 1e-3f);
    EXPECT_NEAR(camAfter.z, rebasedRef.z, 1e-3f);
    EXPECT_NEAR(scene->RebasedToAbsolute(camAfter).x, farPos.x, 1e-3f);
    EXPECT_NEAR(scene->RebasedToAbsolute(camAfter).z, farPos.z, 1e-3f);

    // Hysteresis: an immediate re-check at the now-near reference is a no-op.
    EXPECT_TRUE(IsZeroVec3(scene->MaybeRebaseOrigin(camAfter)));
}

TEST(WorldOriginRebaseTest, SanitizeEnforcesThresholdAboveGridDiagonal)
{
    WorldOriginSettings s;
    s.SnapGridSize = 1024.0f;
    s.RebaseThreshold = 10.0f; // absurdly low — would thrash without the clamp
    SanitizeWorldOriginSettings(s);

    // Must be raised above SnapGridSize * sqrt(3)/2 so a rebase can't re-fire.
    EXPECT_GT(s.RebaseThreshold, s.SnapGridSize * 0.8660254f);

    // Non-finite inputs fall back to defaults, grid floored at 1.
    WorldOriginSettings bad;
    bad.SnapGridSize = std::numeric_limits<f32>::infinity();
    bad.RebaseThreshold = std::numeric_limits<f32>::quiet_NaN();
    SanitizeWorldOriginSettings(bad);
    EXPECT_TRUE(std::isfinite(bad.SnapGridSize));
    EXPECT_TRUE(std::isfinite(bad.RebaseThreshold));
    EXPECT_GE(bad.SnapGridSize, 1.0f);
}
