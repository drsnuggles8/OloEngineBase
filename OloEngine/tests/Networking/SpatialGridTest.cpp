#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/SpatialGrid.h"
#include "OloEngine/Networking/Replication/RelevanceTier.h"
#include "OloEngine/Networking/Replication/NetworkPriorityQueue.h"

using namespace OloEngine;

// ============================================================================
// SpatialGrid Tests
// ============================================================================

TEST(SpatialGrid, InsertAndQuery)
{
    SpatialGrid grid(64.0f);
    grid.InsertOrUpdate(1, { 10.0f, 0.0f, 10.0f });
    grid.InsertOrUpdate(2, { 20.0f, 0.0f, 20.0f });
    grid.InsertOrUpdate(3, { 500.0f, 0.0f, 500.0f });

    auto result = grid.QueryRadius({ 15.0f, 0.0f, 15.0f }, 50.0f);
    EXPECT_EQ(result.size(), 2u); // Should find entities 1 and 2
    EXPECT_TRUE(std::find(result.begin(), result.end(), 1) != result.end());
    EXPECT_TRUE(std::find(result.begin(), result.end(), 2) != result.end());
    EXPECT_TRUE(std::find(result.begin(), result.end(), 3) == result.end());
}

TEST(SpatialGrid, RadiusAccuracy)
{
    SpatialGrid grid(32.0f);
    // Place entity exactly at radius boundary
    grid.InsertOrUpdate(1, { 100.0f, 0.0f, 0.0f });

    // Query from origin with radius 99 — should NOT find it
    auto miss = grid.QueryRadius({ 0.0f, 0.0f, 0.0f }, 99.0f);
    EXPECT_TRUE(miss.empty());

    // Query from origin with radius 101 — should find it
    auto hit = grid.QueryRadius({ 0.0f, 0.0f, 0.0f }, 101.0f);
    EXPECT_EQ(hit.size(), 1u);
    EXPECT_EQ(hit[0], 1u);
}

TEST(SpatialGrid, UpdateMovesEntity)
{
    SpatialGrid grid(64.0f);
    grid.InsertOrUpdate(1, { 0.0f, 0.0f, 0.0f });

    // Entity 1 should be near origin
    auto nearby = grid.QueryRadius({ 0.0f, 0.0f, 0.0f }, 10.0f);
    EXPECT_EQ(nearby.size(), 1u);

    // Move entity far away
    grid.InsertOrUpdate(1, { 1000.0f, 0.0f, 1000.0f });

    // No longer near origin
    auto gone = grid.QueryRadius({ 0.0f, 0.0f, 0.0f }, 10.0f);
    EXPECT_TRUE(gone.empty());

    // Found at new position
    auto found = grid.QueryRadius({ 1000.0f, 0.0f, 1000.0f }, 10.0f);
    EXPECT_EQ(found.size(), 1u);
}

TEST(SpatialGrid, RemoveEntity)
{
    SpatialGrid grid(64.0f);
    grid.InsertOrUpdate(1, { 0.0f, 0.0f, 0.0f });
    EXPECT_EQ(grid.GetEntityCount(), 1u);

    grid.Remove(1);
    EXPECT_EQ(grid.GetEntityCount(), 0u);

    auto result = grid.QueryRadius({ 0.0f, 0.0f, 0.0f }, 100.0f);
    EXPECT_TRUE(result.empty());
}

TEST(SpatialGrid, CellPopulation)
{
    SpatialGrid grid(64.0f);
    // All three entities in the same cell at origin
    grid.InsertOrUpdate(1, { 1.0f, 0.0f, 1.0f });
    grid.InsertOrUpdate(2, { 2.0f, 0.0f, 2.0f });
    grid.InsertOrUpdate(3, { 3.0f, 0.0f, 3.0f });

    EXPECT_EQ(grid.GetCellPopulation({ 0.0f, 0.0f, 0.0f }), 3u);
    EXPECT_EQ(grid.GetCellPopulation({ 1000.0f, 0.0f, 1000.0f }), 0u);
}

// ============================================================================
// RelevanceTier Tests
// ============================================================================

TEST(RelevanceTier, TierAssignment)
{
    RelevanceTierConfig config;
    config.CloseRange = 50.0f;
    config.MidRange = 150.0f;
    config.FarRange = 300.0f;

    // Close range
    EXPECT_EQ(GetRelevanceTier(40.0f * 40.0f, config), ENetworkRelevanceTier::Full);

    // Mid range
    EXPECT_EQ(GetRelevanceTier(100.0f * 100.0f, config), ENetworkRelevanceTier::Reduced);

    // Far range
    EXPECT_EQ(GetRelevanceTier(200.0f * 200.0f, config), ENetworkRelevanceTier::Minimal);
}

TEST(RelevanceTier, UpdateFrequency)
{
    // Full tier: every tick
    EXPECT_TRUE(ShouldUpdateOnTick(ENetworkRelevanceTier::Full, 0));
    EXPECT_TRUE(ShouldUpdateOnTick(ENetworkRelevanceTier::Full, 1));
    EXPECT_TRUE(ShouldUpdateOnTick(ENetworkRelevanceTier::Full, 7));

    // Reduced tier: every 3rd tick
    EXPECT_TRUE(ShouldUpdateOnTick(ENetworkRelevanceTier::Reduced, 0));
    EXPECT_FALSE(ShouldUpdateOnTick(ENetworkRelevanceTier::Reduced, 1));
    EXPECT_FALSE(ShouldUpdateOnTick(ENetworkRelevanceTier::Reduced, 2));
    EXPECT_TRUE(ShouldUpdateOnTick(ENetworkRelevanceTier::Reduced, 3));

    // Minimal tier: every 10th tick
    EXPECT_TRUE(ShouldUpdateOnTick(ENetworkRelevanceTier::Minimal, 0));
    EXPECT_FALSE(ShouldUpdateOnTick(ENetworkRelevanceTier::Minimal, 1));
    EXPECT_TRUE(ShouldUpdateOnTick(ENetworkRelevanceTier::Minimal, 10));
}

// ============================================================================
// NetworkPriorityQueue Tests
// ============================================================================

TEST(NetworkPriorityQueue, PriorityOrdering)
{
    NetworkPriorityQueue queue;

    // Entity 1: close (dist=100), stale (10 ticks) → high priority
    queue.UpdatePriority(1, 100.0f, 10);

    // Entity 2: far (dist=10000), fresh (1 tick) → low priority
    queue.UpdatePriority(2, 10000.0f, 1);

    // Entity 3: mid (dist=1000), very stale (50 ticks) → mid-high priority
    queue.UpdatePriority(3, 1000.0f, 50);

    auto top = queue.GetTopN(3);
    EXPECT_EQ(top.size(), 3u);
    EXPECT_EQ(top[0].EntityUUID, 1u); // Closest + stale = highest
}

TEST(NetworkPriorityQueue, TopNCaps)
{
    NetworkPriorityQueue queue;
    for (u32 i = 1; i <= 10; ++i)
    {
        queue.UpdatePriority(i, static_cast<f32>(i * 100), 1);
    }

    auto top3 = queue.GetTopN(3);
    EXPECT_EQ(top3.size(), 3u);
}

TEST(NetworkPriorityQueue, RemoveEntity)
{
    NetworkPriorityQueue queue;
    queue.UpdatePriority(1, 100.0f, 1);
    queue.UpdatePriority(2, 200.0f, 1);
    EXPECT_EQ(queue.GetCount(), 2u);

    queue.Remove(1);
    EXPECT_EQ(queue.GetCount(), 1u);
}
