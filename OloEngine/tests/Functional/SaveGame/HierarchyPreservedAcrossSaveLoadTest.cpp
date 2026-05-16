#include "OloEnginePCH.h"

// =============================================================================
// HierarchyPreservedAcrossSaveLoadTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene hierarchy × SaveGameSerializer. Save/load is the production "save
//   mid-game" path; if `RelationshipComponent` doesn't round-trip with the
//   rest of the scene, restoring a save produces a flat scene where every
//   entity has lost its parent — vehicles separated from their turrets,
//   characters separated from their attached items. Existing
//   SaveGameIntegrationTest verifies basic component round-trip on a flat
//   scene; the Functional angle adds the parent-child relationships and asserts
//   they survive intact.
//
// Scenario: build a scene with parent-child wired via Entity::SetParent.
// Capture, restore into a fresh Scene. Assert the restored child still
// references the restored parent by UUID.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class HierarchyPreservedAcrossSaveLoadTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Parent = GetScene().CreateEntity("Vehicle");
        m_Parent.GetComponent<TransformComponent>().Translation = { 10.0f, 0.0f, 0.0f };

        m_Child = GetScene().CreateEntity("Turret");
        m_Child.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f }; // local offset
        m_Child.SetParent(m_Parent);

        // Distinct UUIDs for matching across the round-trip.
        m_ParentUUID = m_Parent.GetUUID();
        m_ChildUUID = m_Child.GetUUID();
    }

    Entity m_Parent;
    Entity m_Child;
    UUID m_ParentUUID;
    UUID m_ChildUUID;
};

TEST_F(HierarchyPreservedAcrossSaveLoadTest, ChildRetainsParentReferenceAfterRoundTrip)
{
    // Sanity: hierarchy intact in the source scene.
    ASSERT_TRUE(m_Child.GetParent());
    ASSERT_EQ(m_Child.GetParent().GetUUID(), m_ParentUUID);

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    auto restoredParentOpt = restored->TryGetEntityWithUUID(m_ParentUUID);
    auto restoredChildOpt = restored->TryGetEntityWithUUID(m_ChildUUID);

    ASSERT_TRUE(restoredParentOpt) << "parent entity missing after round-trip";
    ASSERT_TRUE(restoredChildOpt) << "child entity missing after round-trip";

    Entity restoredParent = *restoredParentOpt;
    Entity restoredChild = *restoredChildOpt;

    // The headline assertion: child's parent reference survived.
    Entity restoredChildParent = restoredChild.GetParent();
    EXPECT_TRUE(restoredChildParent)
        << "after save/load, child has no parent — RelationshipComponent did not "
           "round-trip through SaveGameSerializer; every parented entity in a save "
           "would re-load orphaned.";

    EXPECT_EQ(restoredChildParent.GetUUID(), m_ParentUUID)
        << "child's parent UUID after restore (" << static_cast<u64>(restoredChildParent.GetUUID())
        << ") does not match the original (" << static_cast<u64>(m_ParentUUID) << ")";

    // Local transform should also survive (the offset, not the world position).
    const auto& childT = restoredChild.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(childT.x, 0.0f, 1e-3f);
    EXPECT_NEAR(childT.y, 1.0f, 1e-3f);
    EXPECT_NEAR(childT.z, 0.0f, 1e-3f);
}
