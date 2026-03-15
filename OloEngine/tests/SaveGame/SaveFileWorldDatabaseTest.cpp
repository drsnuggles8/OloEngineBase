#include <gtest/gtest.h>

#include "OloEngine/SaveGame/SaveFileWorldDatabase.h"
#include "OloEngine/Networking/MMO/PlayerStatePacket.h"

#include <filesystem>
#include <type_traits>
#include <vector>

using namespace OloEngine;

// SaveFileWorldDatabase requires an active project for disk operations,
// but we can test the in-memory IWorldDatabase contract without one.
// These tests verify the interface compliance by using InMemoryWorldDatabase-like
// operations that don't touch disk.

// For now, we verify Initialize/Shutdown lifecycle and the compile-time
// IWorldDatabase conformance. Full round-trip tests require a project context.

TEST(SaveFileWorldDatabaseTest, ImplementsIWorldDatabase)
{
    // Compile-time check: SaveFileWorldDatabase implements IWorldDatabase
    static_assert(std::is_base_of_v<IWorldDatabase, SaveFileWorldDatabase>);
}

TEST(SaveFileWorldDatabaseTest, DefaultIsNotInitialized)
{
    SaveFileWorldDatabase db;
    EXPECT_FALSE(db.IsInitialized());
}

TEST(SaveFileWorldDatabaseTest, UninitializedOperationsFail)
{
    SaveFileWorldDatabase db;

    PlayerStatePacket state;
    EXPECT_FALSE(db.SavePlayerState(1, state));
    EXPECT_FALSE(db.LoadPlayerState(1, state));
    EXPECT_FALSE(db.DeletePlayerState(1));

    std::vector<u8> data = { 1, 2, 3 };
    EXPECT_FALSE(db.SaveEntityState(100, 1, data));

    std::vector<std::pair<u64, std::vector<u8>>> entities;
    EXPECT_FALSE(db.LoadEntitiesForZone(1, entities));
    EXPECT_FALSE(db.DeleteEntityState(100));

    std::string value;
    EXPECT_FALSE(db.SetWorldState("key", "value"));
    EXPECT_FALSE(db.GetWorldState("key", value));
}
