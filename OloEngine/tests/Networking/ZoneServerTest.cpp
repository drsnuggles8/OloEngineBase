#include <gtest/gtest.h>

#include "OloEngine/Networking/MMO/ZoneDefinition.h"
#include "OloEngine/Networking/MMO/ZoneServer.h"
#include "OloEngine/Networking/MMO/ZoneManager.h"
#include "OloEngine/Networking/MMO/InterZoneMessageBus.h"

using namespace OloEngine;

// ============================================================================
// ZoneServer Tests
// ============================================================================

TEST(ZoneServer, StartStop)
{
    ZoneDefinition def;
    def.ID = 1;
    def.Name = "TestZone";
    def.MaxPlayers = 10;

    ZoneServer server;
    server.Initialize(def);
    EXPECT_FALSE(server.IsRunning());

    server.Start();
    EXPECT_TRUE(server.IsRunning());
    EXPECT_EQ(server.GetZoneID(), 1u);
    EXPECT_EQ(server.GetName(), "TestZone");

    server.Stop();
    EXPECT_FALSE(server.IsRunning());
}

TEST(ZoneServer, AddRemovePlayers)
{
    ZoneDefinition def;
    def.ID = 1;
    def.Name = "PlayerZone";
    def.MaxPlayers = 2;

    ZoneServer server;
    server.Initialize(def);
    server.Start();

    EXPECT_TRUE(server.AddPlayer(100));
    EXPECT_TRUE(server.HasPlayer(100));
    EXPECT_EQ(server.GetPlayerCount(), 1u);

    EXPECT_TRUE(server.AddPlayer(200));
    EXPECT_EQ(server.GetPlayerCount(), 2u);

    // Zone is full now
    EXPECT_TRUE(server.IsFull());
    EXPECT_FALSE(server.AddPlayer(300));

    server.RemovePlayer(100);
    EXPECT_FALSE(server.HasPlayer(100));
    EXPECT_EQ(server.GetPlayerCount(), 1u);
    EXPECT_FALSE(server.IsFull());
}

// ============================================================================
// ZoneManager Tests
// ============================================================================

TEST(ZoneManager, RegisterAndRoutePlayer)
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

    EXPECT_EQ(manager.GetZoneCount(), 2u);

    // Route player to forest
    ZoneID routed = manager.RoutePlayerToZone(1, { 0.0f, 0.0f, 0.0f });
    EXPECT_EQ(routed, 1u);
    EXPECT_EQ(manager.GetPlayerZone(1), 1u);

    // Route player to desert
    ZoneID routed2 = manager.RoutePlayerToZone(2, { 200.0f, 0.0f, 0.0f });
    EXPECT_EQ(routed2, 2u);

    manager.StopAll();
}

TEST(ZoneManager, TransferPlayer)
{
    ZoneManager manager;

    ZoneDefinition zone1;
    zone1.ID = 1;
    zone1.Name = "Zone1";
    zone1.Bounds.Min = { -100.0f, -100.0f, -100.0f };
    zone1.Bounds.Max = { 100.0f, 100.0f, 100.0f };
    manager.RegisterZone(zone1);

    ZoneDefinition zone2;
    zone2.ID = 2;
    zone2.Name = "Zone2";
    zone2.Bounds.Min = { 100.0f, -100.0f, -100.0f };
    zone2.Bounds.Max = { 300.0f, 100.0f, 100.0f };
    manager.RegisterZone(zone2);

    manager.StartAll();

    manager.RoutePlayerToZone(1, { 0.0f, 0.0f, 0.0f });
    EXPECT_EQ(manager.GetPlayerZone(1), 1u);

    EXPECT_TRUE(manager.TransferPlayerToZone(1, 2));
    EXPECT_EQ(manager.GetPlayerZone(1), 2u);

    manager.StopAll();
}

TEST(ZoneManager, MaxPlayersEnforcement)
{
    ZoneManager manager;

    ZoneDefinition def;
    def.ID = 1;
    def.Name = "SmallZone";
    def.Bounds.Min = { -100.0f, -100.0f, -100.0f };
    def.Bounds.Max = { 100.0f, 100.0f, 100.0f };
    def.MaxPlayers = 2;
    manager.RegisterZone(def);
    manager.StartAll();

    EXPECT_EQ(manager.RoutePlayerToZone(1, { 0.0f, 0.0f, 0.0f }), 1u);
    EXPECT_EQ(manager.RoutePlayerToZone(2, { 0.0f, 0.0f, 0.0f }), 1u);
    EXPECT_EQ(manager.RoutePlayerToZone(3, { 0.0f, 0.0f, 0.0f }), 0u); // Full

    manager.StopAll();
}

// ============================================================================
// InterZoneMessageBus Tests
// ============================================================================

TEST(InterZoneMessageBus, PushAndDrainAll)
{
    InterZoneMessageBus bus;

    InterZoneMessage msg1;
    msg1.Type = EInterZoneMessageType::ChatRelay;
    msg1.SourceZoneID = 1;
    msg1.TargetZoneID = 2;
    bus.Push(std::move(msg1));

    InterZoneMessage msg2;
    msg2.Type = EInterZoneMessageType::WorldEvent;
    msg2.SourceZoneID = 2;
    msg2.TargetZoneID = 0; // Broadcast
    bus.Push(std::move(msg2));

    EXPECT_TRUE(bus.HasMessages());
    EXPECT_EQ(bus.GetPendingCount(), 2u);

    auto messages = bus.DrainAll();
    EXPECT_EQ(messages.size(), 2u);
    EXPECT_FALSE(bus.HasMessages());
}

TEST(InterZoneMessageBus, DrainForZone)
{
    InterZoneMessageBus bus;

    InterZoneMessage msg1;
    msg1.TargetZoneID = 1;
    bus.Push(std::move(msg1));

    InterZoneMessage msg2;
    msg2.TargetZoneID = 2;
    bus.Push(std::move(msg2));

    InterZoneMessage msg3;
    msg3.TargetZoneID = 0; // Broadcast — should be picked up by zone 1
    bus.Push(std::move(msg3));

    auto zone1Msgs = bus.DrainForZone(1);
    EXPECT_EQ(zone1Msgs.size(), 2u); // msg1 + msg3 (broadcast)

    // msg2 and msg3 (broadcast, preserved for other zones) should still be in the bus
    EXPECT_EQ(bus.GetPendingCount(), 2u);
}
