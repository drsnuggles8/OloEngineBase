#include "OloEnginePCH.h"

// =============================================================================
// EntityNameMapStaysConsistentTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene.m_EntityNameMap × CreateEntity / DestroyEntity / UpdateEntityName.
//   Gameplay code looks up entities by tag at runtime ("find the player",
//   "find the goal trigger") via FindEntityByName. The name map is updated
//   incrementally — Scene::UpdateEntityName erases the old key and inserts
//   the new one. A regression in any of the three update paths leaves
//   stale tags in the map, so FindEntityByName returns either nothing
//   (false negative) or the wrong entity (false positive). This is the
//   kind of bug only surfaced by exercising create/rename/destroy in
//   sequence under the harness.
//
// Scenario: create three entities, find them by name, rename one, find by
// the new name, destroy one, find returns null. Walks every transition
// the name map's invariants depend on.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class EntityNameMapStaysConsistentTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_A = GetScene().CreateEntity("Alpha");
        m_B = GetScene().CreateEntity("Beta");
        m_C = GetScene().CreateEntity("Gamma");
    }

    Entity m_A, m_B, m_C;
};

TEST_F(EntityNameMapStaysConsistentTest, CreateRenameDestroyKeepsNameLookupCorrect)
{
    // 1. Initial create — all three findable by their original name.
    {
        Entity foundA = GetScene().FindEntityByName("Alpha");
        Entity foundB = GetScene().FindEntityByName("Beta");
        Entity foundC = GetScene().FindEntityByName("Gamma");
        ASSERT_TRUE(foundA);
        ASSERT_TRUE(foundB);
        ASSERT_TRUE(foundC);
        EXPECT_EQ(foundA.GetUUID(), m_A.GetUUID());
        EXPECT_EQ(foundB.GetUUID(), m_B.GetUUID());
        EXPECT_EQ(foundC.GetUUID(), m_C.GetUUID());
    }

    // 2. Rename Beta → "BetaRenamed" via the canonical Scene helper. After
    //    this, the old name should NOT find anything, and the new name
    //    should find the renamed entity.
    {
        const auto oldName = std::string{ "Beta" };
        const auto newName = std::string{ "BetaRenamed" };
        m_B.GetComponent<TagComponent>().Tag = newName;
        GetScene().UpdateEntityName(static_cast<entt::entity>(m_B), oldName, newName);

        Entity foundOld = GetScene().FindEntityByName("Beta");
        Entity foundNew = GetScene().FindEntityByName("BetaRenamed");

        EXPECT_FALSE(foundOld)
            << "stale name 'Beta' still resolves after rename — UpdateEntityName "
               "did not erase the old key from m_EntityNameMap.";
        ASSERT_TRUE(foundNew)
            << "new name 'BetaRenamed' does not resolve — UpdateEntityName did "
               "not insert the new key into m_EntityNameMap.";
        EXPECT_EQ(foundNew.GetUUID(), m_B.GetUUID());
    }

    // 3. Destroy Alpha. After this, the old name should not find anything;
    //    the destroyed entity's UUID lookup should also fail.
    {
        const UUID alphaUUID = m_A.GetUUID();
        GetScene().DestroyEntity(m_A);
        m_A = Entity{};

        Entity stillThere = GetScene().FindEntityByName("Alpha");
        EXPECT_FALSE(stillThere)
            << "destroyed entity 'Alpha' still resolves by name — DestroyEntity "
               "did not erase the entry from m_EntityNameMap.";

        auto byUUID = GetScene().TryGetEntityWithUUID(alphaUUID);
        EXPECT_FALSE(byUUID)
            << "destroyed entity is still findable by UUID";
    }

    // 4. The remaining live entities are unaffected.
    {
        Entity foundC = GetScene().FindEntityByName("Gamma");
        Entity foundB = GetScene().FindEntityByName("BetaRenamed");
        ASSERT_TRUE(foundC);
        ASSERT_TRUE(foundB);
        EXPECT_EQ(foundC.GetUUID(), m_C.GetUUID());
        EXPECT_EQ(foundB.GetUUID(), m_B.GetUUID());
    }
}
