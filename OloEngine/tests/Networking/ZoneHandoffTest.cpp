#include <gtest/gtest.h>

#include "OloEngine/Networking/MMO/PlayerStatePacket.h"
#include "OloEngine/Networking/MMO/ZoneManager.h"
#include "OloEngine/Networking/MMO/ZoneServer.h"

using namespace OloEngine;

// ============================================================================
// PlayerStatePacket Tests
// ============================================================================

TEST(PlayerStatePacket, SerializeDeserializeRoundtrip)
{
    PlayerStatePacket original;
    original.ClientID = 42;
    original.EntityUUID = 12345;
    original.SourceZoneID = 1;
    original.TargetZoneID = 2;
    original.Position = { 10.0f, 20.0f, 30.0f };
    original.Rotation = { 0.1f, 0.2f, 0.3f };
    original.Scale = { 1.5f, 2.0f, 0.5f };
    original.OwnerClientID = 42;
    original.IsReplicated = true;
    original.GameStateBlob = { 0xDE, 0xAD, 0xBE, 0xEF };

    auto buffer = original.Serialize();
    ASSERT_FALSE(buffer.empty());

    auto restored = PlayerStatePacket::Deserialize(buffer.data(), static_cast<i64>(buffer.size()));
    ASSERT_TRUE(restored.has_value());

    EXPECT_EQ(restored->ClientID, 42u);
    EXPECT_EQ(restored->EntityUUID, 12345u);
    EXPECT_EQ(restored->SourceZoneID, 1u);
    EXPECT_EQ(restored->TargetZoneID, 2u);
    EXPECT_FLOAT_EQ(restored->Position.x, 10.0f);
    EXPECT_FLOAT_EQ(restored->Position.y, 20.0f);
    EXPECT_FLOAT_EQ(restored->Position.z, 30.0f);
    EXPECT_FLOAT_EQ(restored->Rotation.x, 0.1f);
    EXPECT_FLOAT_EQ(restored->Rotation.y, 0.2f);
    EXPECT_FLOAT_EQ(restored->Rotation.z, 0.3f);
    EXPECT_FLOAT_EQ(restored->Scale.x, 1.5f);
    EXPECT_FLOAT_EQ(restored->Scale.y, 2.0f);
    EXPECT_FLOAT_EQ(restored->Scale.z, 0.5f);
    EXPECT_EQ(restored->OwnerClientID, 42u);
    EXPECT_TRUE(restored->IsReplicated);
    ASSERT_EQ(restored->GameStateBlob.size(), 4u);
    EXPECT_EQ(restored->GameStateBlob[0], 0xDE);
}

TEST(PlayerStatePacket, EmptyGameStateBlob)
{
    PlayerStatePacket original;
    original.ClientID = 1;
    // Leave GameStateBlob empty

    auto buffer = original.Serialize();
    auto restored = PlayerStatePacket::Deserialize(buffer.data(), static_cast<i64>(buffer.size()));
    ASSERT_TRUE(restored.has_value());

    EXPECT_EQ(restored->ClientID, 1u);
    EXPECT_TRUE(restored->GameStateBlob.empty());
}

// ============================================================================
// Handoff Protocol Tests
// ============================================================================

static ZoneManager CreateTwoZoneManager()
{
    ZoneManager manager;

    ZoneDefinition zone1;
    zone1.ID = 1;
    zone1.Name = "Forest";
    zone1.Bounds.Min = { -100.0f, -100.0f, -100.0f };
    zone1.Bounds.Max = { 100.0f, 100.0f, 100.0f };
    zone1.MaxPlayers = 50;
    manager.RegisterZone(zone1);

    ZoneDefinition zone2;
    zone2.ID = 2;
    zone2.Name = "Desert";
    zone2.Bounds.Min = { 100.0f, -100.0f, -100.0f };
    zone2.Bounds.Max = { 300.0f, 100.0f, 100.0f };
    zone2.MaxPlayers = 50;
    manager.RegisterZone(zone2);

    manager.StartAll();
    return manager;
}

TEST(ZoneHandoff, ThreePhaseHandoffProtocol)
{
    auto manager = CreateTwoZoneManager();

    // Place player in zone 1
    EXPECT_EQ(manager.RoutePlayerToZone(1, { 0.0f, 0.0f, 0.0f }), 1u);

    PlayerStatePacket state;
    state.ClientID = 1;
    state.Position = { 99.0f, 0.0f, 0.0f }; // Near boundary

    // Phase 1: Begin handoff
    u32 txID = manager.BeginHandoff(1, 2, state);
    ASSERT_GT(txID, 0u);
    EXPECT_EQ(manager.GetPlayerZone(1), 1u); // Still in source

    auto* tx = manager.GetHandoff(txID);
    ASSERT_NE(tx, nullptr);
    EXPECT_EQ(tx->State, EHandoffState::Requested);

    // Source zone should mark player as transitioning
    auto* source = manager.GetZone(1);
    EXPECT_TRUE(source->IsPlayerTransitioning(1));

    // Phase 2: Accept handoff
    EXPECT_TRUE(manager.AcceptHandoff(txID));
    tx = manager.GetHandoff(txID);
    EXPECT_EQ(tx->State, EHandoffState::Ready);

    // Phase 3: Complete handoff
    EXPECT_TRUE(manager.CompleteHandoff(txID));
    EXPECT_EQ(manager.GetPlayerZone(1), 2u); // Now in target
    EXPECT_FALSE(source->HasPlayer(1));
    EXPECT_TRUE(manager.GetZone(2)->HasPlayer(1));
}

TEST(ZoneHandoff, HandoffRejectionOnFull)
{
    ZoneManager manager;

    ZoneDefinition zone1;
    zone1.ID = 1;
    zone1.Name = "Source";
    zone1.Bounds.Min = { -100.0f, -100.0f, -100.0f };
    zone1.Bounds.Max = { 100.0f, 100.0f, 100.0f };
    zone1.MaxPlayers = 50;
    manager.RegisterZone(zone1);

    ZoneDefinition zone2;
    zone2.ID = 2;
    zone2.Name = "Target";
    zone2.Bounds.Min = { 100.0f, -100.0f, -100.0f };
    zone2.Bounds.Max = { 300.0f, 100.0f, 100.0f };
    zone2.MaxPlayers = 1; // Only room for 1
    manager.RegisterZone(zone2);

    manager.StartAll();

    // Fill target zone
    manager.RoutePlayerToZone(99, { 200.0f, 0.0f, 0.0f });

    // Place player in zone 1
    manager.RoutePlayerToZone(1, { 0.0f, 0.0f, 0.0f });

    PlayerStatePacket state;
    state.ClientID = 1;

    u32 txID = manager.BeginHandoff(1, 2, state);
    ASSERT_GT(txID, 0u);

    // Accept should fail because target is full, which triggers rejection
    EXPECT_FALSE(manager.AcceptHandoff(txID));

    // Player should still be in zone 1, no longer transitioning
    EXPECT_EQ(manager.GetPlayerZone(1), 1u);
}

// ============================================================================
// Ghost Entity Tests
// ============================================================================

TEST(ZoneHandoff, GhostEntityVisibility)
{
    ZoneDefinition def;
    def.ID = 1;
    def.Name = "Test";
    def.MaxPlayers = 10;

    ZoneServer server;
    server.Initialize(def);
    server.Start();

    // Add a ghost entity
    glm::vec3 pos{ 50.0f, 0.0f, 50.0f };
    server.AddGhostEntity(1000, pos);

    EXPECT_TRUE(server.IsGhostEntity(1000));
    EXPECT_EQ(server.GetGhostEntityCount(), 1u);

    // Ghost entities should appear in spatial grid queries
    auto nearby = server.GetSpatialGrid().QueryRadius(pos, 10.0f);
    EXPECT_EQ(nearby.size(), 1u);

    // Remove ghost entity
    server.RemoveGhostEntity(1000);
    EXPECT_FALSE(server.IsGhostEntity(1000));
    EXPECT_EQ(server.GetGhostEntityCount(), 0u);
}

TEST(ZoneHandoff, PlayerTransitioningState)
{
    ZoneDefinition def;
    def.ID = 1;
    def.Name = "Test";
    def.MaxPlayers = 10;

    ZoneServer server;
    server.Initialize(def);
    server.Start();
    server.AddPlayer(1);

    EXPECT_FALSE(server.IsPlayerTransitioning(1));

    server.SetPlayerTransitioning(1, true);
    EXPECT_TRUE(server.IsPlayerTransitioning(1));

    server.SetPlayerTransitioning(1, false);
    EXPECT_FALSE(server.IsPlayerTransitioning(1));
}
