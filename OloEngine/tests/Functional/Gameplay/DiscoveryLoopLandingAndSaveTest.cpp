#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// DiscoveryLoopLandingAndSaveTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Physics3D (trigger contacts) x Gameplay (DiscoverySystem) x SaveGame. This
//   is the discovery loop added for issue #881: a landing-trigger volume
//   (Rigidbody3DComponent{IsTrigger=true} + BoxCollider3DComponent on a
//   DiscoverableComponent entity) registers a visit into the toucher's
//   DiscoveredSetComponent, driven by DiscoverySystem polling the physics
//   scene's active contact pairs each tick (Scene::UpdateDiscovery).
//
// Three things the issue's acceptance bullets call out explicitly, and that
// "tests green, screen wrong" experience says are easy to get silently
// wrong for a set-based discovery mechanic:
//   1. Landing registers exactly once, and RESTING in the trigger across many
//      ticks (the same contact pair reported every frame) must not
//      double-count — a naive "insert on every contact" would.
//   2. The discovered set survives a save/reload round-trip byte-for-byte.
//   3. A save captured BEFORE DiscoveredSetComponent existed on an entity
//      (simulated here by simply not adding the component) still loads
//      without error — the component is just absent post-restore, exactly
//      like any other optional component; no version-gating is needed for a
//      brand-new component type (only for a new FIELD on one already shipped).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr const char* kIslandName = "TestIsland";
    constexpr const char* kBoatName = "TestBoat";

    // A trigger volume large enough that the fully-overlapping boat below
    // stays inside it every tick (no gravity fall-out, no drift-out).
    void AddLandingTrigger(Entity island)
    {
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Static;
        body.m_IsTrigger = true;
        island.AddComponent<Rigidbody3DComponent>(body);

        BoxCollider3DComponent collider;
        collider.m_HalfExtents = { 10.0f, 10.0f, 10.0f };
        island.AddComponent<BoxCollider3DComponent>(collider);
    }

    void AddBoatBody(Entity boat)
    {
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_DisableGravity = true; // stays put at the overlap position
        boat.AddComponent<Rigidbody3DComponent>(body);

        BoxCollider3DComponent collider;
        collider.m_HalfExtents = { 1.0f, 1.0f, 1.0f };
        boat.AddComponent<BoxCollider3DComponent>(collider);
    }
} // namespace

class DiscoveryLoopLandingAndSaveTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Island = GetScene().CreateEntity(kIslandName);
        m_Island.AddComponent<DiscoverableComponent>().m_DisplayName = "Test Island";
        AddLandingTrigger(m_Island);

        // Fully overlapping the trigger from frame zero — the scenario is
        // "already landed / resting", which is the case most likely to
        // double-count under a naive per-contact-event implementation.
        m_Boat = GetScene().CreateEntity(kBoatName);
        m_Boat.AddComponent<DiscoveredSetComponent>();
        AddBoatBody(m_Boat);

        EnablePhysics3D();
    }

    Entity m_Island;
    Entity m_Boat;
};

TEST_F(DiscoveryLoopLandingAndSaveTest, LandingRegistersOnceAndRestingDoesNotDoubleCount)
{
    // A handful of ticks for the physics contact to be detected and the
    // Discovery system (post-physics-fence) to process it.
    TickFor(/*totalSeconds=*/0.5f);

    ASSERT_TRUE(m_Boat.HasComponent<DiscoveredSetComponent>());
    const auto& discovered = m_Boat.GetComponent<DiscoveredSetComponent>().m_Discovered;
    ASSERT_EQ(discovered.size(), 1u) << "landing on the island must register exactly one discovery";
    EXPECT_EQ(static_cast<u64>(discovered.front()), static_cast<u64>(m_Island.GetUUID()));

    // Keep resting inside the trigger for many more ticks — GetActiveContactPairs()
    // reports the same pair every one of them. The set must stay at size 1.
    TickFor(/*totalSeconds=*/2.0f);
    EXPECT_EQ(m_Boat.GetComponent<DiscoveredSetComponent>().m_Discovered.size(), 1u)
        << "resting in the landing trigger across repeated ticks double-counted the same island";
}

TEST_F(DiscoveryLoopLandingAndSaveTest, DiscoveredSetSurvivesSaveReloadRoundTrip)
{
    TickFor(/*totalSeconds=*/0.5f);
    ASSERT_EQ(m_Boat.GetComponent<DiscoveredSetComponent>().m_Discovered.size(), 1u);

    const auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restoredScene = Scene::Create();
    restoredScene->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restoredScene, payload));

    Entity restoredBoat = restoredScene->FindEntityByName(kBoatName);
    ASSERT_TRUE(restoredBoat) << "boat entity missing from the restored scene";
    ASSERT_TRUE(restoredBoat.HasComponent<DiscoveredSetComponent>())
        << "DiscoveredSetComponent dropped by the save-game round-trip — check the "
           "REGISTER_SAVE_COMPONENT / generated SAVE_COMPONENT+TRY_LOAD_COMPONENT lists";

    const auto& restoredDiscovered = restoredBoat.GetComponent<DiscoveredSetComponent>().m_Discovered;
    ASSERT_EQ(restoredDiscovered.size(), 1u) << "quit and reopen must keep the discovered set intact";
    EXPECT_EQ(static_cast<u64>(restoredDiscovered.front()), static_cast<u64>(m_Island.GetUUID()));
}

// Simulates a save file written BEFORE this game added DiscoveredSetComponent
// to the boat: the captured payload simply has no such component on that
// entity. Restoring it into a scene that fully knows about the component
// type must still succeed — a brand-new component type needs no version
// gate, only a field added to an already-shipped component does (see
// HasFieldsSince usage elsewhere in SaveGameComponentSerializer.cpp).
TEST_F(DiscoveryLoopLandingAndSaveTest, OldSaveWithoutDiscoveredSetComponentStillLoads)
{
    ASSERT_TRUE(m_Boat.HasComponent<DiscoveredSetComponent>());
    m_Boat.RemoveComponent<DiscoveredSetComponent>();

    const auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restoredScene = Scene::Create();
    restoredScene->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restoredScene, payload))
        << "a save predating DiscoveredSetComponent must still load cleanly";

    Entity restoredBoat = restoredScene->FindEntityByName(kBoatName);
    ASSERT_TRUE(restoredBoat) << "boat entity missing from the restored scene";
    EXPECT_FALSE(restoredBoat.HasComponent<DiscoveredSetComponent>())
        << "a component absent from the save payload must stay absent after restore, not "
           "default-construct silently";
}
