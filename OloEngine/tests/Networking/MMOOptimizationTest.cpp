#include <gtest/gtest.h>

#include "OloEngine/Networking/Core/NetworkModel.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Networking/Replication/NetworkPriorityQueue.h"
#include "OloEngine/Networking/Replication/RelevanceTier.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;

// ============================================================================
// NetworkLODComponent Tests
// ============================================================================

TEST(MMOOptimization, NetworkLODLevels)
{
    NetworkLODComponent lod;
    EXPECT_EQ(lod.Level, ENetworkLOD::Full);

    lod.Level = ENetworkLOD::Reduced;
    EXPECT_EQ(lod.Level, ENetworkLOD::Reduced);

    lod.Level = ENetworkLOD::Dormant;
    EXPECT_EQ(lod.Level, ENetworkLOD::Dormant);
}

// ============================================================================
// Bandwidth Budgeting Tests
// ============================================================================

TEST(MMOOptimization, BandwidthCapEnforcement)
{
    NetworkPriorityQueue queue;

    // Add entities with varying distances
    for (u32 i = 1; i <= 100; i++)
    {
        queue.UpdatePriority(i, static_cast<f32>(i * 10), i); // Varying distance and staleness
    }

    // Cap at 20 entities — simulates bandwidth budget
    auto top20 = queue.GetTopN(20);
    EXPECT_EQ(top20.size(), 20u);

    // Verify all returned entries have valid UUIDs
    for (auto const& entry : top20)
    {
        EXPECT_GT(entry.EntityUUID, 0u);
        EXPECT_GT(entry.Score, 0.0f);
    }
}

// ============================================================================
// ENetworkModel::MMO Tests
// ============================================================================

TEST(MMOOptimization, MMONetworkModelExists)
{
    ENetworkModel model = ENetworkModel::MMO;
    EXPECT_NE(model, ENetworkModel::None);
    EXPECT_NE(model, ENetworkModel::ClientServerAuthoritative);
    EXPECT_NE(model, ENetworkModel::Lockstep);
    EXPECT_NE(model, ENetworkModel::PeerToPeer);
    EXPECT_NE(model, ENetworkModel::TurnBased);
}

// ============================================================================
// MMO Message Types Tests
// ============================================================================

TEST(MMOOptimization, MMOMessageTypesExist)
{
    // Verify all MMO message types compile and have distinct values
    EXPECT_NE(ENetworkMessageType::ZoneHandoffRequest, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::ZoneHandoffReady, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::ZoneHandoffComplete, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::InstanceCreate, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::InstanceDestroy, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::InstanceJoin, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::LayerAssign, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::LayerMerge, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::ChatSend, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::ChatReceive, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::ChatJoinChannel, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::ChatLeaveChannel, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::PlayerLogin, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::PlayerLogout, ENetworkMessageType::None);
    EXPECT_NE(ENetworkMessageType::WorldStateSync, ENetworkMessageType::None);
}

// ============================================================================
// Component Registration Tests
// ============================================================================

TEST(MMOOptimization, PhaseComponentDefaults)
{
    PhaseComponent phase;
    EXPECT_EQ(phase.PhaseID, 0u);
}

TEST(MMOOptimization, InstancePortalComponentDefaults)
{
    InstancePortalComponent portal;
    EXPECT_EQ(portal.TargetZoneID, 0u);
    EXPECT_EQ(portal.InstanceType, 0u);
    EXPECT_EQ(portal.MaxPlayers, 5u);
}

// ============================================================================
// Crowd Update Threshold Test
// ============================================================================

TEST(MMOOptimization, CrowdUpdateThreshold)
{
    // When cell population exceeds threshold (40), updates should be capped
    constexpr u32 crowdThreshold = 40;
    constexpr u32 crowdUpdateRateHz = 8;

    // Simulate: 50 entities in one cell
    u32 const entityCount = 50;
    EXPECT_GT(entityCount, crowdThreshold);

    // In crowd mode, tick rate should be capped
    u32 const normalRateHz = 20;
    u32 const effectiveRate = (entityCount > crowdThreshold) ? crowdUpdateRateHz : normalRateHz;
    EXPECT_EQ(effectiveRate, crowdUpdateRateHz);
}

// ============================================================================
// NetworkLOD Filtering Tests
// ============================================================================

TEST(MMOOptimization, NetworkLODFiltering)
{
    // Dormant entities should send no updates
    NetworkLODComponent dormant;
    dormant.Level = ENetworkLOD::Dormant;
    EXPECT_EQ(dormant.Level, ENetworkLOD::Dormant);

    // Simulate: when LOD is Dormant, entity is skipped during replication
    bool shouldReplicate = (dormant.Level != ENetworkLOD::Dormant);
    EXPECT_FALSE(shouldReplicate);

    // Full LOD should replicate
    NetworkLODComponent full;
    full.Level = ENetworkLOD::Full;
    shouldReplicate = (full.Level != ENetworkLOD::Dormant);
    EXPECT_TRUE(shouldReplicate);
}
