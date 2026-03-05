#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Initialize Populates Metadata
// =============================================================================

TEST(CommandPacket, InitializePopulatesType)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticDrawMeshCommand(100, 200, 0.5f, 42);

    PacketMetadata meta;
    meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 100, 200, 500);
    meta.m_DebugName = "TestMesh";
    meta.m_GroupID = 7;

    auto* packet = allocator.CreateCommandPacket(cmd, meta);
    ASSERT_NE(packet, nullptr);

    EXPECT_EQ(packet->GetCommandType(), CommandType::DrawMesh);
    EXPECT_STREQ(packet->GetCommandTypeString(), "DrawMesh");
    EXPECT_EQ(packet->GetMetadata().m_GroupID, 7u);
    EXPECT_STREQ(packet->GetMetadata().m_DebugName, "TestMesh");
}

TEST(CommandPacket, InitializeSetsCommandType)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticClearCommand();

    auto* packet = allocator.CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    EXPECT_EQ(packet->GetCommandType(), CommandType::Clear);
}

TEST(CommandPacket, DrawMeshPacketStoresShaderAndMaterialKeys)
{
    CommandAllocator allocator;

    auto cmd = MakeSyntheticDrawMeshCommand(42, 0, 0.5f);

    PacketMetadata meta;
    meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 42, 99, 500);

    auto* packet = allocator.CreateCommandPacket(cmd, meta);
    ASSERT_NE(packet, nullptr);

    // Sort key preserves shader/material IDs set in metadata
    EXPECT_EQ(packet->GetMetadata().m_SortKey.GetShaderID(), 42u);
    EXPECT_EQ(packet->GetMetadata().m_SortKey.GetMaterialID(), 99u);
}

// =============================================================================
// Get/Set Command Data
// =============================================================================

TEST(CommandPacket, GetCommandDataReturnsCorrectType)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticDrawMeshCommand(10, 20, 1.0f, 99);

    auto* packet = allocator.CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    const auto* retrieved = packet->GetCommandData<DrawMeshCommand>();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->materialDataIndex, static_cast<u16>(20));
    EXPECT_EQ(retrieved->entityID, 99);
    EXPECT_EQ(retrieved->indexCount, 36u);
}

TEST(CommandPacket, CommandSizeMatchesType)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticViewportCommand(0, 0, 800, 600);

    auto* packet = allocator.CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    EXPECT_EQ(packet->GetCommandSize(), sizeof(SetViewportCommand));
}

// =============================================================================
// Multiple Packets Can Be Created Independently
// =============================================================================

TEST(CommandPacket, MultiplePacketsAreIndependent)
{
    CommandAllocator allocator;

    auto cmd1 = MakeSyntheticClearCommand();
    auto cmd2 = MakeSyntheticViewportCommand();
    auto cmd3 = MakeSyntheticDepthTestCommand();

    auto* p1 = allocator.CreateCommandPacket(cmd1);
    auto* p2 = allocator.CreateCommandPacket(cmd2);
    auto* p3 = allocator.CreateCommandPacket(cmd3);

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    // Each packet is independent — no linked list coupling
    EXPECT_EQ(p1->GetCommandType(), CommandType::Clear);
    EXPECT_EQ(p2->GetCommandType(), CommandType::SetViewport);
    EXPECT_EQ(p3->GetCommandType(), CommandType::SetDepthTest);

    // All are distinct objects
    EXPECT_NE(p1, p2);
    EXPECT_NE(p2, p3);
    EXPECT_NE(p1, p3);
}

// =============================================================================
// Comparison Operator (< for sorting)
// =============================================================================

TEST(CommandPacket, ComparisonFollowsDrawKeySorting)
{
    CommandAllocator allocator;

    auto cmd1 = MakeSyntheticDrawMeshCommand();
    auto cmd2 = MakeSyntheticDrawMeshCommand();

    PacketMetadata meta1;
    meta1.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 100);

    PacketMetadata meta2;
    meta2.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 200);

    auto* p1 = allocator.CreateCommandPacket(cmd1, meta1);
    auto* p2 = allocator.CreateCommandPacket(cmd2, meta2);

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    // The packet with the higher-priority DrawKey should sort first
    // (DrawKey::operator< returns m_Key > other.m_Key)
    bool p1LessThanP2 = *p1 < *p2;
    bool p2LessThanP1 = *p2 < *p1;

    // At least one direction must be true (they shouldn't be equal since depths differ)
    EXPECT_NE(p1LessThanP2, p2LessThanP1)
        << "Packets with different keys should have a strict ordering";
}

// =============================================================================
// CanBatchWith
// =============================================================================

TEST(CommandPacket, CanBatchWithSameTypeSameKeyFields)
{
    CommandAllocator allocator;

    auto cmd1 = MakeSyntheticDrawMeshCommand(1, 1, 0.5f);
    auto cmd2 = MakeSyntheticDrawMeshCommand(1, 1, 0.6f);

    PacketMetadata meta;
    meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, 100);

    auto* p1 = allocator.CreateCommandPacket(cmd1, meta);
    auto* p2 = allocator.CreateCommandPacket(cmd2, meta);

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    // Two DrawMesh commands with same shader/material should be batchable
    EXPECT_TRUE(p1->CanBatchWith(*p2));
}

TEST(CommandPacket, CannotBatchDifferentCommandTypes)
{
    CommandAllocator allocator;

    auto cmd1 = MakeSyntheticClearCommand();
    auto cmd2 = MakeSyntheticViewportCommand();

    auto* p1 = allocator.CreateCommandPacket(cmd1);
    auto* p2 = allocator.CreateCommandPacket(cmd2);

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    EXPECT_FALSE(p1->CanBatchWith(*p2));
}

// =============================================================================
// Clone Deep Copies
// =============================================================================

TEST(CommandPacket, CloneDeepCopiesData)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticDrawMeshCommand(42, 99, 1.5f, 7);

    PacketMetadata meta;
    meta.m_SortKey = MakeSyntheticOpaqueKey(1, ViewLayerType::TwoD, 42, 99, 500);
    meta.m_DebugName = "OriginalMesh";
    meta.m_GroupID = 3;

    auto* original = allocator.CreateCommandPacket(cmd, meta);
    ASSERT_NE(original, nullptr);

    auto* clone = original->Clone(allocator);
    ASSERT_NE(clone, nullptr);
    EXPECT_NE(clone, original) << "Clone must be a different pointer";

    // Verify the clone has the same data
    EXPECT_EQ(clone->GetCommandType(), original->GetCommandType());
    EXPECT_EQ(clone->GetCommandSize(), original->GetCommandSize());
    EXPECT_EQ(clone->GetMetadata().m_SortKey.GetKey(), original->GetMetadata().m_SortKey.GetKey());
    EXPECT_EQ(clone->GetMetadata().m_GroupID, original->GetMetadata().m_GroupID);

    // Verify command data is copied
    const auto* origData = original->GetCommandData<DrawMeshCommand>();
    const auto* cloneData = clone->GetCommandData<DrawMeshCommand>();
    ASSERT_NE(cloneData, nullptr);
    EXPECT_EQ(cloneData->materialDataIndex, origData->materialDataIndex);
    EXPECT_EQ(cloneData->entityID, origData->entityID);
}

// =============================================================================
// Sort Key Immutability
// =============================================================================

TEST(CommandPacket, SortKeyPreservedFromMetadata)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticDrawMeshCommand(42, 7);

    // Caller sets sort key explicitly — the standard production path
    PacketMetadata meta;
    meta.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 0xABCD, 0x1234, 500);
    auto* packet = allocator.CreateCommandPacket(cmd, meta);
    ASSERT_NE(packet, nullptr);

    // Sort key must be exactly what the caller provided
    EXPECT_EQ(packet->GetMetadata().m_SortKey.GetShaderID(), 0xABCD);
    EXPECT_EQ(packet->GetMetadata().m_SortKey.GetMaterialID(), 0x1234);
    EXPECT_EQ(packet->GetMetadata().m_SortKey.GetDepth(), 500u);
}

TEST(CommandPacket, DefaultMetadataKeepsSortKeyZeroed)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticDrawMeshCommand(99, 5);

    // No metadata provided — sort key fields stay zero (no auto-derivation)
    auto* packet = allocator.CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    EXPECT_EQ(packet->GetMetadata().m_SortKey.GetShaderID(), 0u);
    EXPECT_EQ(packet->GetMetadata().m_SortKey.GetMaterialID(), 0u);
}

// =============================================================================
// AllocatePacketWithCommand (Placement-new path)
// =============================================================================

TEST(CommandPacket, AllocatePacketWithCommandPath)
{
    CommandAllocator allocator;

    auto* packet = allocator.AllocatePacketWithCommand<DrawMeshCommand>();
    ASSERT_NE(packet, nullptr);

    // Get the command pointer and populate it
    auto* cmd = packet->GetCommandData<DrawMeshCommand>();
    ASSERT_NE(cmd, nullptr);

    cmd->header.type = CommandType::DrawMesh;
    cmd->materialDataIndex = 77;
    cmd->entityID = 42;
    cmd->indexCount = 100;

    // Verify the data persists
    const auto* readBack = packet->GetCommandData<DrawMeshCommand>();
    EXPECT_EQ(readBack->materialDataIndex, static_cast<u16>(77));
    EXPECT_EQ(readBack->entityID, 42);
    EXPECT_EQ(readBack->indexCount, 100u);
}

// =============================================================================
// Metadata Properties
// =============================================================================

TEST(CommandPacket, MetadataDependencyAndStaticFlags)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticClearCommand();

    PacketMetadata meta;
    meta.m_DependsOnPrevious = true;
    meta.m_IsStatic = true;
    meta.m_ExecutionOrder = 42;

    auto* packet = allocator.CreateCommandPacket(cmd, meta);
    ASSERT_NE(packet, nullptr);

    const auto& m = packet->GetMetadata();
    EXPECT_TRUE(m.m_DependsOnPrevious);
    EXPECT_TRUE(m.m_IsStatic);
    EXPECT_EQ(m.m_ExecutionOrder, 42u);
}
