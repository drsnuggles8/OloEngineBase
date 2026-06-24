#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/NetworkInterestManager.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <algorithm>
#include <vector>

using namespace OloEngine;

class InterestManagerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Scene = CreateScope<Scene>();
        m_Manager = NetworkInterestManager();
    }

    Scope<Scene> m_Scene;
    NetworkInterestManager m_Manager;
};

TEST_F(InterestManagerTest, EntitiesWithoutInterestComponentAreAlwaysRelevant)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "NoInterest");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;

    auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
    bool found = std::ranges::find(relevant, 100u) != relevant.end();
    EXPECT_TRUE(found);
}

TEST_F(InterestManagerTest, ZeroRadiusIsAlwaysRelevant)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "ZeroRadius");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;
    auto& interest = e.AddComponent<NetworkInterestComponent>();
    interest.RelevanceRadius = 0.0f;

    m_Manager.SetClientPosition(1, { 9999.0f, 0.0f, 0.0f });
    auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
    bool found = std::ranges::find(relevant, 100u) != relevant.end();
    EXPECT_TRUE(found);
}

TEST_F(InterestManagerTest, DistanceFilteringWorks)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Near");
    auto& tc = e.GetComponent<TransformComponent>();
    tc.Translation = { 10.0f, 0.0f, 0.0f };
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;
    auto& interest = e.AddComponent<NetworkInterestComponent>();
    interest.RelevanceRadius = 5.0f;

    // Client is at origin — entity is 10 units away, radius is 5
    m_Manager.SetClientPosition(1, { 0.0f, 0.0f, 0.0f });
    auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
    bool found = std::ranges::find(relevant, 100u) != relevant.end();
    EXPECT_FALSE(found); // Too far

    // Move client closer
    m_Manager.SetClientPosition(1, { 8.0f, 0.0f, 0.0f });
    relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
    found = std::ranges::find(relevant, 100u) != relevant.end();
    EXPECT_TRUE(found); // Within 5 units
}

TEST_F(InterestManagerTest, GroupFilteringWorks)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "GroupEntity");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;
    auto& interest = e.AddComponent<NetworkInterestComponent>();
    interest.InterestGroup = 5;

    // Client not subscribed to group 5
    m_Manager.SetClientInterestGroups(1, { 1, 2 });
    auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
    bool found = std::ranges::find(relevant, 100u) != relevant.end();
    EXPECT_FALSE(found);

    // Subscribe client to group 5
    m_Manager.SetClientInterestGroups(1, { 1, 5 });
    relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
    found = std::ranges::find(relevant, 100u) != relevant.end();
    EXPECT_TRUE(found);
}

TEST_F(InterestManagerTest, DefaultGroupIsAlwaysIncluded)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "DefaultGroup");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;
    auto& interest = e.AddComponent<NetworkInterestComponent>();
    interest.InterestGroup = 0; // Default group

    // Client has no explicit group subscriptions
    auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
    bool found = std::ranges::find(relevant, 100u) != relevant.end();
    EXPECT_TRUE(found);
}

TEST_F(InterestManagerTest, NonReplicatedEntitiesAreExcluded)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "NoReplicate");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = false;

    auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
    bool found = std::ranges::find(relevant, 100u) != relevant.end();
    EXPECT_FALSE(found);
}

TEST_F(InterestManagerTest, IsEntityRelevantMatchesGetRelevantEntities)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Test");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;

    bool relevant = m_Manager.IsEntityRelevant(1, 100, *m_Scene);
    EXPECT_TRUE(relevant);
}

// The core behaviour-preservation contract: the grid-accelerated query (after UpdateSpatialGrid)
// must return EXACTLY the same set as the full-scene scan (a missed cell = a dropped replication).
TEST_F(InterestManagerTest, GridFilteredResultsMatchFullScan)
{
    // cellSize is 64, so spread positions across several cells and mix every relevance category.
    auto makeEntity = [&](u64 id, const char* name, const glm::vec3& pos)
    {
        Entity e = m_Scene->CreateEntityWithUUID(UUID(id), name);
        e.GetComponent<TransformComponent>().Translation = pos;
        return e;
    };

    // 1: replicated, no interest component, far away → always relevant.
    {
        auto e = makeEntity(1, "AlwaysFar", { 5000.0f, 0.0f, 0.0f });
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
    }
    // 2: replicated, RelevanceRadius 0, far away → always relevant.
    {
        auto e = makeEntity(2, "ZeroRadiusFar", { 4000.0f, 0.0f, 0.0f });
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
        e.AddComponent<NetworkInterestComponent>().RelevanceRadius = 0.0f;
    }
    // 3: distance-filterable, in range.
    {
        auto e = makeEntity(3, "NearInRange", { 10.0f, 0.0f, 0.0f });
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
        e.AddComponent<NetworkInterestComponent>().RelevanceRadius = 50.0f;
    }
    // 4: distance-filterable, out of range.
    {
        auto e = makeEntity(4, "FarOutRange", { 1000.0f, 0.0f, 0.0f });
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
        e.AddComponent<NetworkInterestComponent>().RelevanceRadius = 50.0f;
    }
    // 5: in range, but in a group the client is NOT subscribed to.
    {
        auto e = makeEntity(5, "WrongGroup", { 12.0f, 0.0f, 0.0f });
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
        auto& i = e.AddComponent<NetworkInterestComponent>();
        i.RelevanceRadius = 50.0f;
        i.InterestGroup = 5;
    }
    // 6: in range, in a group the client IS subscribed to.
    {
        auto e = makeEntity(6, "RightGroup", { 14.0f, 0.0f, 0.0f });
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
        auto& i = e.AddComponent<NetworkInterestComponent>();
        i.RelevanceRadius = 50.0f;
        i.InterestGroup = 3;
    }
    // 7: non-replicated → excluded entirely (not in grid, not in always-relevant).
    {
        auto e = makeEntity(7, "NonReplicated", { 8.0f, 0.0f, 0.0f });
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = false;
    }
    // 8: no NetworkIdentityComponent at all + no interest → always relevant, far away.
    {
        makeEntity(8, "NoIdentityFar", { 3000.0f, 0.0f, 0.0f });
    }

    constexpr u32 client = 1;
    auto configure = [&](NetworkInterestManager& m)
    {
        m.SetClientPosition(client, { 0.0f, 0.0f, 0.0f });
        m.SetClientInterestGroups(client, { 3 });
    };

    // Full-scan reference: never call UpdateSpatialGrid → GetRelevantEntities walks the scene.
    NetworkInterestManager fullScan;
    configure(fullScan);
    auto expected = fullScan.GetRelevantEntities(client, *m_Scene);

    // Grid-accelerated: populate the grid first → GetRelevantEntities uses the grid path.
    NetworkInterestManager grid;
    configure(grid);
    grid.UpdateSpatialGrid(*m_Scene);
    auto actual = grid.GetRelevantEntities(client, *m_Scene);

    std::ranges::sort(expected);
    std::ranges::sort(actual);
    EXPECT_EQ(expected, actual);

    // Spell out the expected membership so a fault shared by BOTH paths is still caught.
    std::vector<u64> wanted{ 1u, 2u, 3u, 6u, 8u };
    std::ranges::sort(wanted);
    EXPECT_EQ(actual, wanted);

    // The grid now holds only the distance-filterable entities (3, 4, 5, 6); always-relevant
    // entities (1, 2, 8) live in the flat list, non-replicated (7) is dropped.
    EXPECT_EQ(grid.GetSpatialGrid().GetEntityCount(), 4u);
}

// Sweep the client across clusters: the grid path must agree with the full scan at every position,
// including positions where whole clusters fall out of range.
TEST_F(InterestManagerTest, GridMatchesFullScanAcrossClientPositions)
{
    auto make = [&](u64 id, const glm::vec3& pos, f32 radius)
    {
        Entity e = m_Scene->CreateEntityWithUUID(UUID(id), "E");
        e.GetComponent<TransformComponent>().Translation = pos;
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
        e.AddComponent<NetworkInterestComponent>().RelevanceRadius = radius;
    };
    // Cluster A near the origin.
    make(10, { 0.0f, 0.0f, 0.0f }, 40.0f);
    make(11, { 20.0f, 10.0f, 0.0f }, 40.0f);
    // Cluster B ~500 units away (separate grid cells).
    make(20, { 500.0f, 0.0f, 0.0f }, 40.0f);
    make(21, { 520.0f, 5.0f, 0.0f }, 40.0f);
    // An always-relevant beacon far from both clusters.
    {
        Entity e = m_Scene->CreateEntityWithUUID(UUID(30), "Beacon");
        e.GetComponent<TransformComponent>().Translation = { 9000.0f, 0.0f, 0.0f };
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
    }

    NetworkInterestManager grid;
    grid.UpdateSpatialGrid(*m_Scene);

    const std::vector<glm::vec3> positions = {
        { 0.0f, 0.0f, 0.0f }, { 510.0f, 0.0f, 0.0f }, { 250.0f, 0.0f, 0.0f }, { -9999.0f, 0.0f, 0.0f }
    };
    for (const auto& pos : positions)
    {
        NetworkInterestManager scan;
        scan.SetClientPosition(1, pos);
        grid.SetClientPosition(1, pos);

        auto expected = scan.GetRelevantEntities(1, *m_Scene);
        auto actual = grid.GetRelevantEntities(1, *m_Scene);
        std::ranges::sort(expected);
        std::ranges::sort(actual);
        EXPECT_EQ(expected, actual) << "client at (" << pos.x << ", " << pos.y << ", " << pos.z << ")";
    }
}

// A single entity with a pathologically large radius must not turn every query into a giant cell
// walk — the grid path declines and the full scan still yields the correct set.
TEST_F(InterestManagerTest, HugeRelevanceRadiusFallsBackToFullScanCorrectly)
{
    auto make = [&](u64 id, const glm::vec3& pos, f32 radius)
    {
        Entity e = m_Scene->CreateEntityWithUUID(UUID(id), "E");
        e.GetComponent<TransformComponent>().Translation = pos;
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
        e.AddComponent<NetworkInterestComponent>().RelevanceRadius = radius;
    };
    make(1, { 100.0f, 0.0f, 0.0f }, 100000.0f); // huge radius → grid would span too many cells
    make(2, { 200000.0f, 0.0f, 0.0f }, 50.0f);  // small radius, far → out of range

    NetworkInterestManager scan;
    scan.SetClientPosition(1, { 0.0f, 0.0f, 0.0f });
    auto expected = scan.GetRelevantEntities(1, *m_Scene);

    NetworkInterestManager grid;
    grid.SetClientPosition(1, { 0.0f, 0.0f, 0.0f });
    grid.UpdateSpatialGrid(*m_Scene); // maxRadius huge → grid query declines, full scan is used
    auto actual = grid.GetRelevantEntities(1, *m_Scene);

    std::ranges::sort(expected);
    std::ranges::sort(actual);
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(actual, (std::vector<u64>{ 1u })); // 1 (dist 100 < 100000) in; 2 (dist 200000 > 50) out
}

// IsEntityRelevant must agree with GetRelevantEntities membership for every entity, whether or not
// the grid has been populated.
TEST_F(InterestManagerTest, IsEntityRelevantAgreesWithGetRelevantEntities)
{
    auto make = [&](u64 id, const glm::vec3& pos, bool hasInterest, f32 radius, u32 group, bool replicated)
    {
        Entity e = m_Scene->CreateEntityWithUUID(UUID(id), "E");
        e.GetComponent<TransformComponent>().Translation = pos;
        e.AddComponent<NetworkIdentityComponent>().IsReplicated = replicated;
        if (hasInterest)
        {
            auto& i = e.AddComponent<NetworkInterestComponent>();
            i.RelevanceRadius = radius;
            i.InterestGroup = group;
        }
    };
    make(1, { 10.0f, 0.0f, 0.0f }, true, 50.0f, 0, true);   // in range
    make(2, { 1000.0f, 0.0f, 0.0f }, true, 50.0f, 0, true); // out of range
    make(3, { 10.0f, 0.0f, 0.0f }, true, 50.0f, 5, true);   // wrong group (client not subscribed)
    make(4, { 5000.0f, 0.0f, 0.0f }, false, 0.0f, 0, true); // always relevant
    make(5, { 8.0f, 0.0f, 0.0f }, true, 50.0f, 0, false);   // non-replicated

    const std::vector<u64> ids = { 1u, 2u, 3u, 4u, 5u };

    auto check = [&](NetworkInterestManager& m)
    {
        m.SetClientPosition(1, { 0.0f, 0.0f, 0.0f });
        m.SetClientInterestGroups(1, { 1, 2 }); // not subscribed to group 5
        auto relevant = m.GetRelevantEntities(1, *m_Scene);
        for (u64 id : ids)
        {
            bool const inList = std::ranges::find(relevant, id) != relevant.end();
            EXPECT_EQ(inList, m.IsEntityRelevant(1, id, *m_Scene)) << "entity " << id;
        }
        EXPECT_FALSE(m.IsEntityRelevant(1, 9999u, *m_Scene)); // unknown UUID is never relevant
    };

    NetworkInterestManager scanMgr;
    check(scanMgr);

    NetworkInterestManager gridMgr;
    gridMgr.UpdateSpatialGrid(*m_Scene);
    check(gridMgr);
}

// An entity that loses its TransformComponent AFTER UpdateSpatialGrid snapshotted it must drop out
// of every query path identically — the full scan only sees entities with a TransformComponent, so
// the grid path (which fetches stale snapshot UUIDs by lookup) and IsEntityRelevant must agree and,
// crucially, must not deref the now-missing component. Guards against the grid path and full scan
// diverging (and an assert/UB on GetComponent<TransformComponent>) when the snapshot goes stale.
TEST_F(InterestManagerTest, EntityLosingTransformAfterSnapshotDropsOutOfAllPaths)
{
    // 1: distance-filterable (positive radius) → lives in the grid snapshot.
    Entity distance = m_Scene->CreateEntityWithUUID(UUID(1), "Distance");
    distance.GetComponent<TransformComponent>().Translation = { 10.0f, 0.0f, 0.0f };
    distance.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
    distance.AddComponent<NetworkInterestComponent>().RelevanceRadius = 50.0f;

    // 2: always-relevant (no interest) → lives in the flat snapshot list.
    Entity always = m_Scene->CreateEntityWithUUID(UUID(2), "Always");
    always.GetComponent<TransformComponent>().Translation = { 20.0f, 0.0f, 0.0f };
    always.AddComponent<NetworkIdentityComponent>().IsReplicated = true;

    NetworkInterestManager grid;
    grid.SetClientPosition(1, { 0.0f, 0.0f, 0.0f });
    grid.UpdateSpatialGrid(*m_Scene); // snapshot taken while both still have a TransformComponent

    // Now strip the transforms — the snapshot still references the UUIDs, but the live entities no
    // longer satisfy the GetAllEntitiesWith<IDComponent, TransformComponent> filter.
    distance.RemoveComponent<TransformComponent>();
    always.RemoveComponent<TransformComponent>();

    // Full scan never considers them (no TransformComponent) → empty expected set.
    NetworkInterestManager scan;
    scan.SetClientPosition(1, { 0.0f, 0.0f, 0.0f });
    auto expected = scan.GetRelevantEntities(1, *m_Scene);

    // Grid path must not crash on the stale snapshot and must produce the same (empty) set.
    auto actual = grid.GetRelevantEntities(1, *m_Scene);

    std::ranges::sort(expected);
    std::ranges::sort(actual);
    EXPECT_EQ(expected, actual);
    EXPECT_TRUE(actual.empty());

    // IsEntityRelevant already guards the missing transform; confirm both managers agree with it.
    EXPECT_FALSE(grid.IsEntityRelevant(1, 1u, *m_Scene));
    EXPECT_FALSE(grid.IsEntityRelevant(1, 2u, *m_Scene));
}
