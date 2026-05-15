#include "OloEnginePCH.h"

// =============================================================================
// MultipleScenesCoexistTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene × Physics3D × global state. Editor workflows often have two scenes
//   alive simultaneously (e.g. main scene + a streamed sub-scene, or
//   server/client scenes in a co-process net test, or a preview scene plus
//   the active runtime scene). Each Scene constructs its own JoltScene that
//   touches process-wide singletons (`JPH::Factory::sInstance`,
//   `JPH::RegisterTypes`). If those singletons aren't safe under concurrent
//   ownership the second scene corrupts the first — and the bug only shows
//   up when two scenes run simultaneously, never in a single-scene test.
//
// Scenario: two independent Scenes, each with its own falling sphere on its
// own static floor. Tick them in lockstep. Assert each sphere lands in its
// own scene's coordinate space without contaminating the other.
//
// NOTE: this test deliberately exercises a path the engine may not fully
// support today. If it fails, the failure documents the gap.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    Entity MakeFloor(Scene& scene)
    {
        auto floor = scene.CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& fb = floor.AddComponent<Rigidbody3DComponent>();
        fb.m_Type = BodyType3D::Static;
        auto& fc = floor.AddComponent<BoxCollider3DComponent>();
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        return floor;
    }

    Entity MakeFallingSphere(Scene& scene, glm::vec3 pos)
    {
        auto e = scene.CreateEntity("Sphere");
        e.GetComponent<TransformComponent>().Translation = pos;
        SphereCollider3DComponent col;
        col.m_Radius = 0.5f;
        e.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        e.AddComponent<Rigidbody3DComponent>(body);
        return e;
    }
} // namespace

class MultipleScenesCoexistTest : public FunctionalTest
{
  protected:
    // The harness owns m_Scene (Scene A). We construct a second scene
    // manually inside the test for the coexistence assertion.
    void BuildScene() override
    {
        // Scene A: harness-owned. Sphere starts at y=5 directly above origin.
        m_FloorA = MakeFloor(GetScene());
        m_SphereA = MakeFallingSphere(GetScene(), { 0.0f, 5.0f, 0.0f });
        EnablePhysics3D();
    }

    Entity m_FloorA;
    Entity m_SphereA;
};

TEST_F(MultipleScenesCoexistTest, BothScenesTickAndLandIndependently)
{
    // Scene B: owned by this test, distinct from the harness's scene.
    Ref<Scene> sceneB = Scene::Create();
    sceneB->SetRenderingEnabled(false);

    Entity floorB = MakeFloor(*sceneB);
    // Different starting position so a coordinate leak between scenes would
    // be visible (sphere B should not see floor A or vice versa).
    Entity sphereB = MakeFallingSphere(*sceneB, { 10.0f, 7.0f, 0.0f });

    sceneB->OnPhysics3DStart();

    const f32 dt = 1.0f / 60.0f;
    constexpr f32 kSimSeconds = 3.0f;
    const u32 frames = static_cast<u32>(kSimSeconds / dt);

    // Tick BOTH scenes per frame, alternating so any cross-scene global state
    // (e.g. a shared JPH::Factory) is exercised in both directions.
    for (u32 i = 0; i < frames; ++i)
    {
        GetScene().OnUpdateRuntime(dt);
        sceneB->OnUpdateRuntime(dt);
    }

    const f32 yA = m_SphereA.GetComponent<TransformComponent>().Translation.y;
    const f32 yB = sphereB.GetComponent<TransformComponent>().Translation.y;
    const f32 xA = m_SphereA.GetComponent<TransformComponent>().Translation.x;
    const f32 xB = sphereB.GetComponent<TransformComponent>().Translation.x;

    EXPECT_TRUE(std::isfinite(yA) && std::isfinite(yB));

    // Both spheres should rest on their respective floors (radius 0.5, top
    // y=0.5). Generous tolerance because Jolt's iterative solver settles
    // around the contact point, not exactly at it.
    EXPECT_NEAR(yA, 0.5f, 0.15f) << "Scene A sphere did not land; y=" << yA;
    EXPECT_NEAR(yB, 0.5f, 0.15f) << "Scene B sphere did not land; y=" << yB;

    // Spheres started at different x and should keep their x — proves bodies
    // didn't get cross-wired between the two JoltScenes.
    EXPECT_NEAR(xA, 0.0f, 0.5f) << "Scene A sphere drifted from origin; x=" << xA;
    EXPECT_NEAR(xB, 10.0f, 0.5f) << "Scene B sphere drifted from x=10; x=" << xB;

    // Clean up scene B before the test exits, otherwise the global Jolt
    // factory shutdown order is undefined relative to the harness's scene.
    sceneB->OnPhysics3DStop();
    sceneB.Reset();
}
