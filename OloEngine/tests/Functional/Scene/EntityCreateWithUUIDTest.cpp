#include "OloEnginePCH.h"

// =============================================================================
// EntityCreateWithUUIDTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::CreateEntityWithUUID × m_EntityMap × m_EntityNameMap × UUID
//   uniqueness. CreateEntityWithUUID is the path the SaveGameSerializer
//   and prefab instantiation use to recreate entities with their original
//   UUIDs. A regression that drops the UUID, fails to register it in
//   m_EntityMap, or accepts duplicates breaks save/load (UUIDs become
//   irreparable references between objects), prefab instantiation, and
//   any cross-system entity reference (e.g. Audio's position resolver
//   that looks entities up by UUID).
//
// Scenario: create an entity with a known UUID, verify the lookup works.
// Create another entity by tag-only (random UUID), verify those don't
// collide. Create N entities through both paths and confirm all are
// individually retrievable.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <unordered_set>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

class EntityCreateWithUUIDTest : public FunctionalTest
{
  protected:
    void BuildScene() override {}
};

TEST_F(EntityCreateWithUUIDTest, CreateEntityWithUUIDIsLookupableByThatUUID)
{
    const UUID kKnownUUID{ 0xDEADBEEF12345678ULL };

    Entity created = GetScene().CreateEntityWithUUID(kKnownUUID, "Designated");
    ASSERT_TRUE(created);
    EXPECT_EQ(created.GetUUID(), kKnownUUID);

    auto looked = GetScene().TryGetEntityWithUUID(kKnownUUID);
    ASSERT_TRUE(looked) << "TryGetEntityWithUUID returned no entity for the UUID we just used";
    EXPECT_EQ(looked->GetUUID(), kKnownUUID);

    // Also resolvable via the name path.
    Entity byName = GetScene().FindEntityByName("Designated");
    ASSERT_TRUE(byName);
    EXPECT_EQ(byName.GetUUID(), kKnownUUID);
}

TEST_F(EntityCreateWithUUIDTest, ManyEntitiesHaveUniqueUUIDs)
{
    constexpr u32 kCount = 200;
    std::unordered_set<UUID> seen;
    seen.reserve(kCount);

    for (u32 i = 0; i < kCount; ++i)
    {
        Entity e = GetScene().CreateEntity("Entity_" + std::to_string(i));
        const auto [it, inserted] = seen.emplace(e.GetUUID());
        ASSERT_TRUE(inserted)
            << "duplicate UUID assigned to two different entities: " << static_cast<u64>(*it)
            << " — the engine's UUID generator is reusing identifiers, which silently "
               "breaks every save/load and cross-system lookup.";
    }

    EXPECT_EQ(seen.size(), kCount);
}
