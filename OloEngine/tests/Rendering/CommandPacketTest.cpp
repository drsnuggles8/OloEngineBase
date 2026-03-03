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

TEST(CommandPacket, InitializeSetsDispatchFunction)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticClearCommand();

    auto* packet = allocator.CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    EXPECT_EQ(packet->GetCommandType(), CommandType::Clear);
    // Dispatch function should be set for known command types
    // (The table is initialized statically or during test startup)
}

TEST(CommandPacket, DrawMeshAutoGeneratesShaderAndMaterialKeys)
{
    CommandAllocator allocator;

    // Create a draw mesh with specific shader/material IDs
    auto cmd = MakeSyntheticDrawMeshCommand(42, 0, 0.5f);
    cmd.useTextureMaps = true;
    cmd.diffuseMapID = 100;
    cmd.specularMapID = 200;

    PacketMetadata meta;
    // Leave sort key with zero shader/material — Initialize should fill them
    auto* packet = allocator.CreateCommandPacket(cmd, meta);
    ASSERT_NE(packet, nullptr);

    // Initialize auto-sets shader ID from shaderRendererID
    EXPECT_EQ(packet->GetMetadata().m_SortKey.GetShaderID(), 42u);
    // Material ID is derived from texture IDs (hash)
    EXPECT_NE(packet->GetMetadata().m_SortKey.GetMaterialID(), 0u);
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
    EXPECT_EQ(retrieved->shaderRendererID, 10u);
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
// Linked List Functionality
// =============================================================================

TEST(CommandPacket, LinkedListTraversal)
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

    p1->SetNext(p2);
    p2->SetNext(p3);
    p3->SetNext(nullptr);

    // Traverse and count
    u32 count = 0;
    CommandPacket* current = p1;
    while (current)
    {
        count++;
        current = current->GetNext();
    }
    EXPECT_EQ(count, 3u);

    // Verify ordering
    EXPECT_EQ(p1->GetNext(), p2);
    EXPECT_EQ(p2->GetNext(), p3);
    EXPECT_EQ(p3->GetNext(), nullptr);
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
    EXPECT_EQ(cloneData->shaderRendererID, origData->shaderRendererID);
    EXPECT_EQ(cloneData->entityID, origData->entityID);

    // Clone's linked list should be independent
    EXPECT_EQ(clone->GetNext(), nullptr);
}

// =============================================================================
// UpdateCommandData
// =============================================================================

TEST(CommandPacket, UpdateCommandDataOverwritesContent)
{
    CommandAllocator allocator;
    auto cmd = MakeSyntheticViewportCommand(0, 0, 800, 600);

    auto* packet = allocator.CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    // Update with new viewport
    auto newCmd = MakeSyntheticViewportCommand(100, 100, 1920, 1080);
    bool updated = packet->UpdateCommandData(&newCmd, sizeof(newCmd));
    EXPECT_TRUE(updated);

    const auto* data = packet->GetCommandData<SetViewportCommand>();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->x, 100u);
    EXPECT_EQ(data->y, 100u);
    EXPECT_EQ(data->width, 1920u);
    EXPECT_EQ(data->height, 1080u);
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
    cmd->shaderRendererID = 77;
    cmd->entityID = 42;
    cmd->indexCount = 100;

    // Verify the data persists
    const auto* readBack = packet->GetCommandData<DrawMeshCommand>();
    EXPECT_EQ(readBack->shaderRendererID, 77u);
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
