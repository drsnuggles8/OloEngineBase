#include "OloEnginePCH.h"

// =============================================================================
// DuplicateTagFindableTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::FindEntityByName × m_EntityNameMap (multimap). The name map
//   is an `unordered_multimap`, so two entities with the same tag is a
//   legitimate scenario (e.g., several "Enemy" entities). Gameplay code
//   that calls `FindEntityByName("Enemy")` on a duplicated tag gets ONE
//   of them — which one is unspecified, but the lookup must not crash
//   or return null. This test pins down that contract: with two
//   duplicates, the lookup returns *some* match whose UUID is one of
//   the two we created, and either deletion path keeps the other live
//   under the same name.
//
// Scenario:
//   1. Create two entities both named "Enemy". Lookup returns one of them.
//   2. Destroy that one. Lookup returns the OTHER one (the survivor).
//   3. Destroy the survivor. Lookup returns null.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <unordered_set>

using namespace OloEngine;
using namespace OloEngine::Functional;

class DuplicateTagFindableTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_E1 = GetScene().CreateEntity("Enemy");
        m_E2 = GetScene().CreateEntity("Enemy");
    }

    Entity m_E1, m_E2;
};

TEST_F(DuplicateTagFindableTest, LookupAlwaysReturnsALiveEntityUntilTheLastDuplicateIsGone)
{
    const UUID u1 = m_E1.GetUUID();
    const UUID u2 = m_E2.GetUUID();
    ASSERT_NE(u1, u2) << "two CreateEntity calls produced colliding UUIDs";

    // Phase 1: lookup returns one of the two. We don't care which — just
    // that it's a valid entity and its UUID is u1 or u2.
    Entity found = GetScene().FindEntityByName("Enemy");
    ASSERT_TRUE(found) << "FindEntityByName returned null even though two 'Enemy' entities exist";
    const std::unordered_set<UUID> validUUIDs{ u1, u2 };
    EXPECT_TRUE(validUUIDs.count(found.GetUUID()))
        << "FindEntityByName returned UUID "
        << static_cast<u64>(found.GetUUID())
        << " which matches neither of our two 'Enemy' entities — m_EntityNameMap is corrupted.";

    // Phase 2: destroy the found one. Lookup must return the OTHER.
    const UUID destroyedUUID = found.GetUUID();
    GetScene().DestroyEntity(found);

    Entity survivor = GetScene().FindEntityByName("Enemy");
    ASSERT_TRUE(survivor)
        << "After destroying one 'Enemy', FindEntityByName returned null — "
           "DestroyEntity erased BOTH entries from the multimap (over-erasure bug).";
    EXPECT_NE(survivor.GetUUID(), destroyedUUID)
        << "lookup returned the destroyed entity's UUID";
    EXPECT_TRUE(validUUIDs.count(survivor.GetUUID()));

    // Phase 3: destroy the survivor — the name should no longer resolve.
    GetScene().DestroyEntity(survivor);

    Entity nothing = GetScene().FindEntityByName("Enemy");
    EXPECT_FALSE(nothing)
        << "after destroying the last 'Enemy', the name still resolves — "
           "DestroyEntity left a stale entry in m_EntityNameMap.";
}
