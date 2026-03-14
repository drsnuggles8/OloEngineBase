#include <gtest/gtest.h>

#include "OloEngine/Networking/Persistence/InMemoryWorldDatabase.h"
#include "OloEngine/Networking/Persistence/WorldPersistenceManager.h"

using namespace OloEngine;

// ============================================================================
// InMemoryWorldDatabase Tests
// ============================================================================

TEST(WorldDatabase, SaveLoadPlayerRoundtrip)
{
    InMemoryWorldDatabase db;
    db.Initialize(":memory:");

    PlayerStatePacket state;
    state.ClientID = 1;
    state.EntityUUID = 12345;
    state.Position = { 10.0f, 20.0f, 30.0f };
    state.OwnerClientID = 1;
    state.GameStateBlob = { 0xAB, 0xCD };

    EXPECT_TRUE(db.SavePlayerState(100, state));
    EXPECT_EQ(db.GetPlayerCount(), 1u);

    PlayerStatePacket loaded;
    EXPECT_TRUE(db.LoadPlayerState(100, loaded));
    EXPECT_EQ(loaded.ClientID, 1u);
    EXPECT_EQ(loaded.EntityUUID, 12345u);
    EXPECT_FLOAT_EQ(loaded.Position.x, 10.0f);
    EXPECT_EQ(loaded.GameStateBlob.size(), 2u);

    // Non-existent player
    PlayerStatePacket notFound;
    EXPECT_FALSE(db.LoadPlayerState(999, notFound));
}

TEST(WorldDatabase, DeletePlayer)
{
    InMemoryWorldDatabase db;
    db.Initialize(":memory:");

    PlayerStatePacket state;
    state.ClientID = 1;
    db.SavePlayerState(100, state);

    EXPECT_TRUE(db.DeletePlayerState(100));
    EXPECT_EQ(db.GetPlayerCount(), 0u);
    EXPECT_FALSE(db.DeletePlayerState(100)); // Already deleted
}

TEST(WorldDatabase, SaveLoadEntityState)
{
    InMemoryWorldDatabase db;
    db.Initialize(":memory:");

    std::vector<u8> data1 = { 1, 2, 3 };
    std::vector<u8> data2 = { 4, 5, 6 };

    EXPECT_TRUE(db.SaveEntityState(100, 1, data1));
    EXPECT_TRUE(db.SaveEntityState(200, 1, data2));
    EXPECT_TRUE(db.SaveEntityState(300, 2, data1)); // Different zone

    std::vector<std::pair<u64, std::vector<u8>>> entities;
    EXPECT_TRUE(db.LoadEntitiesForZone(1, entities));
    EXPECT_EQ(entities.size(), 2u);

    std::vector<std::pair<u64, std::vector<u8>>> zone2Entities;
    EXPECT_TRUE(db.LoadEntitiesForZone(2, zone2Entities));
    EXPECT_EQ(zone2Entities.size(), 1u);
}

TEST(WorldDatabase, WorldState)
{
    InMemoryWorldDatabase db;
    db.Initialize(":memory:");

    EXPECT_TRUE(db.SetWorldState("server.start_time", "2024-01-01"));
    EXPECT_TRUE(db.SetWorldState("world_boss.status", "alive"));

    std::string value;
    EXPECT_TRUE(db.GetWorldState("server.start_time", value));
    EXPECT_EQ(value, "2024-01-01");

    EXPECT_FALSE(db.GetWorldState("nonexistent", value));
}

TEST(WorldDatabase, InitShutdown)
{
    InMemoryWorldDatabase db;
    EXPECT_FALSE(db.IsInitialized());

    db.Initialize(":memory:");
    EXPECT_TRUE(db.IsInitialized());

    db.SavePlayerState(1, {});
    EXPECT_EQ(db.GetPlayerCount(), 1u);

    db.Shutdown();
    EXPECT_FALSE(db.IsInitialized());
    EXPECT_EQ(db.GetPlayerCount(), 0u); // Cleared on shutdown
}

// ============================================================================
// WorldPersistenceManager Tests
// ============================================================================

TEST(WorldPersistenceManager, DirtyEntityTracking)
{
    InMemoryWorldDatabase db;
    db.Initialize(":memory:");

    WorldPersistenceManager mgr;
    mgr.Initialize(&db, 5.0f);

    EXPECT_EQ(mgr.GetDirtyCount(), 0u);

    mgr.MarkDirty(100);
    mgr.MarkDirty(200);
    EXPECT_EQ(mgr.GetDirtyCount(), 2u);
    EXPECT_TRUE(mgr.IsDirty(100));
    EXPECT_FALSE(mgr.IsDirty(999));
}

TEST(WorldPersistenceManager, AutoSaveOnInterval)
{
    InMemoryWorldDatabase db;
    db.Initialize(":memory:");

    WorldPersistenceManager mgr;
    mgr.Initialize(&db, 1.0f); // 1 second interval for fast test

    mgr.MarkDirty(100);
    EXPECT_EQ(mgr.GetDirtyCount(), 1u);

    // Tick for less than interval — should not save
    mgr.Tick(0.5f);
    EXPECT_EQ(mgr.GetDirtyCount(), 1u);

    // Tick past interval — should auto-save
    mgr.Tick(0.6f);
    EXPECT_EQ(mgr.GetDirtyCount(), 0u); // Cleared after save
}

TEST(WorldPersistenceManager, SaveAndLoadEntity)
{
    InMemoryWorldDatabase db;
    db.Initialize(":memory:");

    WorldPersistenceManager mgr;
    mgr.Initialize(&db);

    mgr.MarkDirty(100);
    std::vector<u8> data = { 0xAA, 0xBB };
    EXPECT_TRUE(mgr.SaveEntity(100, 1, data));
    EXPECT_FALSE(mgr.IsDirty(100)); // No longer dirty after save

    std::vector<std::pair<u64, std::vector<u8>>> entities;
    EXPECT_TRUE(mgr.LoadEntitiesForZone(1, entities));
    EXPECT_EQ(entities.size(), 1u);
    EXPECT_EQ(entities[0].first, 100u);
}

TEST(WorldPersistenceManager, PlayerPersistence)
{
    InMemoryWorldDatabase db;
    db.Initialize(":memory:");

    WorldPersistenceManager mgr;
    mgr.Initialize(&db);

    PlayerStatePacket state;
    state.ClientID = 42;
    state.Position = { 1.0f, 2.0f, 3.0f };

    EXPECT_TRUE(mgr.SavePlayer(1, state));

    PlayerStatePacket loaded;
    EXPECT_TRUE(mgr.LoadPlayer(1, loaded));
    EXPECT_EQ(loaded.ClientID, 42u);
    EXPECT_FLOAT_EQ(loaded.Position.y, 2.0f);
}
