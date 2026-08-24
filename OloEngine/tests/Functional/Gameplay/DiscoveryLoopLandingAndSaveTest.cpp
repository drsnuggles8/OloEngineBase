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

// =============================================================================
// The objective marker must target the nearest UNDISCOVERED landmark by its
// TRIGGER CENTRE, not by the landmark entity's raw TransformComponent — Drift's
// islands are authored with the entity origin at a tile CORNER (a
// BoxCollider3DComponent::m_Offset centres the trigger on the landmass), so
// comparing raw Translation distances silently picks whichever island's
// corner is nearest, not whichever island actually is. Caught via live-editor
// verification (the marker targeted a visibly farther island); this test
// pins the fix at the ECS level without needing a live GL context.
// =============================================================================
class DiscoveryLoopObjectiveMarkerUsesTriggerCentreTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // "Near" island: its ENTITY sits far from the discoverer, but its
        // trigger (Translation + Offset) is close.
        m_Near = GetScene().CreateEntity("NearByCentre");
        m_Near.GetComponent<TransformComponent>().Translation = { 500.0f, 0.0f, 0.0f };
        m_Near.AddComponent<DiscoverableComponent>().m_DisplayName = "Near";
        {
            Rigidbody3DComponent body;
            body.m_Type = BodyType3D::Static;
            body.m_IsTrigger = true;
            m_Near.AddComponent<Rigidbody3DComponent>(body);

            BoxCollider3DComponent collider;
            collider.m_HalfExtents = { 5.0f, 5.0f, 5.0f };
            collider.m_Offset = { -490.0f, 0.0f, 0.0f }; // trigger centre at (10, 0, 0)
            m_Near.AddComponent<BoxCollider3DComponent>(collider);
        }

        // "Far" island: its ENTITY sits close to the discoverer (no offset),
        // but its trigger centre is genuinely farther than Near's.
        m_Far = GetScene().CreateEntity("FarByCentre");
        m_Far.GetComponent<TransformComponent>().Translation = { 20.0f, 0.0f, 0.0f };
        m_Far.AddComponent<DiscoverableComponent>().m_DisplayName = "Far";
        {
            Rigidbody3DComponent body;
            body.m_Type = BodyType3D::Static;
            body.m_IsTrigger = true;
            m_Far.AddComponent<Rigidbody3DComponent>(body);

            BoxCollider3DComponent collider;
            collider.m_HalfExtents = { 5.0f, 5.0f, 5.0f };
            // No offset — trigger centre stays at the entity's own (20, 0, 0).
            m_Far.AddComponent<BoxCollider3DComponent>(collider);
        }

        m_Discoverer = GetScene().CreateEntity("Discoverer");
        m_Discoverer.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        m_Discoverer.AddComponent<DiscoveredSetComponent>();

        m_Marker = GetScene().CreateEntity("Marker");
        m_Marker.AddComponent<DiscoveryObjectiveMarkerComponent>();
        m_Marker.AddComponent<UIWorldAnchorComponent>();

        // No EnablePhysics3D(): UpdateObjectiveUI's nearest-landmark scan reads
        // components directly and needs no physics contacts to exercise it.
    }

    Entity m_Near;
    Entity m_Far;
    Entity m_Discoverer;
    Entity m_Marker;
};

TEST_F(DiscoveryLoopObjectiveMarkerUsesTriggerCentreTest, MarkerTargetsNearestByTriggerCentreNotEntityOrigin)
{
    RunFrames(1);

    ASSERT_TRUE(m_Marker.HasComponent<UIWorldAnchorComponent>());
    const auto& anchor = m_Marker.GetComponent<UIWorldAnchorComponent>();
    EXPECT_EQ(static_cast<u64>(anchor.m_TargetEntity), static_cast<u64>(m_Near.GetUUID()))
        << "marker targeted the island whose entity origin is nearest (raw-Translation distance) "
           "instead of the one whose trigger CENTRE is actually nearest";

    // WorldOffset must be relative to the target's own Translation, i.e. it
    // should reproduce the (Offset + lift) that lands on the trigger centre,
    // not a flat authored constant that assumed a zero collider offset.
    EXPECT_FLOAT_EQ(anchor.m_WorldOffset.x, -490.0f);
    EXPECT_GT(anchor.m_WorldOffset.y, 0.0f) << "marker should sit lifted above the trigger centre, not buried in it";
}
