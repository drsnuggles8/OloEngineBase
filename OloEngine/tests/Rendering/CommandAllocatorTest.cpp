#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <vector>
#include <thread>
#include <numeric>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Basic Allocation
// =============================================================================

TEST(CommandAllocator, AllocateReturnsNonNull)
{
    CommandAllocator allocator;

    void* mem = allocator.AllocateCommandMemory(128);
    ASSERT_NE(mem, nullptr);
}

TEST(CommandAllocator, AllocateMultipleNonOverlapping)
{
    CommandAllocator allocator;

    void* a = allocator.AllocateCommandMemory(64);
    void* b = allocator.AllocateCommandMemory(64);
    void* c = allocator.AllocateCommandMemory(64);

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    // All pointers must be distinct
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
}

// =============================================================================
// Alignment Guarantee
// =============================================================================

TEST(CommandAllocator, AlignmentIs16Byte)
{
    CommandAllocator allocator;

    // Allocate many objects of varying sizes and check alignment
    for (sizet size : {1, 7, 16, 33, 64, 128, 255, 512, 1024})
    {
        void* mem = allocator.AllocateCommandMemory(size);
        ASSERT_NE(mem, nullptr) << "Failed to allocate " << size << " bytes";

        auto addr = reinterpret_cast<std::uintptr_t>(mem);
        EXPECT_EQ(addr % CommandAllocator::COMMAND_ALIGNMENT, 0u)
            << "Allocation of " << size << " bytes not aligned to "
            << CommandAllocator::COMMAND_ALIGNMENT << " bytes. Address: 0x"
            << std::hex << addr << std::dec;
    }
}

// =============================================================================
// Multi-Block Allocation (exceeding 64KB block)
// =============================================================================

TEST(CommandAllocator, MultiBlockAllocation)
{
    CommandAllocator allocator;

    // Each block is 64KB. Allocate enough 1KB chunks to span multiple blocks.
    constexpr sizet chunkSize = 1024;
    constexpr sizet numChunks = 128; // 128KB total > 64KB block
    std::vector<void*> pointers;
    pointers.reserve(numChunks);

    for (sizet i = 0; i < numChunks; ++i)
    {
        void* mem = allocator.AllocateCommandMemory(chunkSize);
        ASSERT_NE(mem, nullptr) << "Allocation " << i << " failed";
        pointers.push_back(mem);
    }

    // Verify all pointers are unique
    for (sizet i = 0; i < pointers.size(); ++i)
    {
        for (sizet j = i + 1; j < pointers.size(); ++j)
        {
            EXPECT_NE(pointers[i], pointers[j])
                << "Pointers " << i << " and " << j << " overlap!";
        }
    }

    EXPECT_EQ(allocator.GetAllocationCount(), numChunks);
}

// =============================================================================
// Reset Reuses Memory
// =============================================================================

TEST(CommandAllocator, ResetReusesMemory)
{
    CommandAllocator allocator;

    // Allocate some memory
    void* first = allocator.AllocateCommandMemory(256);
    ASSERT_NE(first, nullptr);
    sizet allocated1 = allocator.GetTotalAllocated();

    // Reset
    allocator.Reset();
    EXPECT_EQ(allocator.GetAllocationCount(), 0u);

    // Allocate again — should reuse existing blocks
    void* second = allocator.AllocateCommandMemory(256);
    ASSERT_NE(second, nullptr);

    // Total allocated (block-level) should not have grown significantly
    sizet allocated2 = allocator.GetTotalAllocated();
    EXPECT_LE(allocated2, allocated1 + CommandAllocator::DEFAULT_BLOCK_SIZE)
        << "Memory grew unexpectedly after Reset";
}

// =============================================================================
// CreateCommandPacket Integration
// =============================================================================

TEST(CommandAllocator, CreateCommandPacketProducesValidPacket)
{
    CommandAllocator allocator;

    auto cmd = MakeSyntheticDrawMeshCommand(50, 60, 2.0f, 123);
    PacketMetadata meta;
    meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 50, 60, 400);

    CommandPacket* packet = allocator.CreateCommandPacket(cmd, meta);
    ASSERT_NE(packet, nullptr);

    EXPECT_EQ(packet->GetCommandType(), CommandType::DrawMesh);
    EXPECT_EQ(packet->GetCommandSize(), sizeof(DrawMeshCommand));

    const auto* data = packet->GetCommandData<DrawMeshCommand>();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->shaderRendererID, 50u);
    EXPECT_EQ(data->entityID, 123);
}

// =============================================================================
// AllocatePacketWithCommand (Placement-New Path)
// =============================================================================

TEST(CommandAllocator, AllocatePacketWithCommandPlacementNew)
{
    CommandAllocator allocator;

    PacketMetadata meta;
    meta.m_GroupID = 99;
    auto* packet = allocator.AllocatePacketWithCommand<SetViewportCommand>(meta);
    ASSERT_NE(packet, nullptr);

    auto* cmd = packet->GetCommandData<SetViewportCommand>();
    ASSERT_NE(cmd, nullptr);

    // The command should be default-constructed
    cmd->header.type = CommandType::SetViewport;
    cmd->x = 10;
    cmd->y = 20;
    cmd->width = 1920;
    cmd->height = 1080;

    // Read back
    EXPECT_EQ(cmd->x, 10u);
    EXPECT_EQ(cmd->width, 1920u);
    EXPECT_EQ(packet->GetMetadata().m_GroupID, 99u);
}

// =============================================================================
// Stress Test — Large Number of Allocations
// =============================================================================

TEST(CommandAllocator, StressTestManyAllocations)
{
    CommandAllocator allocator;

    constexpr sizet numAllocations = 10000;

    for (sizet i = 0; i < numAllocations; ++i)
    {
        auto cmd = MakeSyntheticClearCommand();
        CommandPacket* packet = allocator.CreateCommandPacket(cmd);
        ASSERT_NE(packet, nullptr) << "Failed at allocation " << i;
    }

    EXPECT_EQ(allocator.GetAllocationCount(), numAllocations);

    // Reset and verify we can do it again
    allocator.Reset();
    EXPECT_EQ(allocator.GetAllocationCount(), 0u);

    for (sizet i = 0; i < numAllocations; ++i)
    {
        auto cmd = MakeSyntheticClearCommand();
        CommandPacket* packet = allocator.CreateCommandPacket(cmd);
        ASSERT_NE(packet, nullptr) << "Failed at post-reset allocation " << i;
    }

    EXPECT_EQ(allocator.GetAllocationCount(), numAllocations);
}

// =============================================================================
// Allocation Count Tracking
// =============================================================================

TEST(CommandAllocator, AllocationCountTracksCorrectly)
{
    CommandAllocator allocator;

    EXPECT_EQ(allocator.GetAllocationCount(), 0u);

    allocator.AllocateCommandMemory(64);
    EXPECT_EQ(allocator.GetAllocationCount(), 1u);

    allocator.AllocateCommandMemory(128);
    EXPECT_EQ(allocator.GetAllocationCount(), 2u);

    allocator.AllocateCommandMemory(256);
    EXPECT_EQ(allocator.GetAllocationCount(), 3u);

    allocator.Reset();
    EXPECT_EQ(allocator.GetAllocationCount(), 0u);
}

// =============================================================================
// Constants Are Sensible
// =============================================================================

TEST(CommandAllocator, ConstantsAreSensible)
{
    EXPECT_GE(CommandAllocator::DEFAULT_BLOCK_SIZE, 4096u)
        << "Block size should be at least 4KB";
    EXPECT_GE(CommandAllocator::MAX_COMMAND_SIZE, sizeof(DrawMeshCommand))
        << "MAX_COMMAND_SIZE must fit the largest command";
    EXPECT_EQ(CommandAllocator::COMMAND_ALIGNMENT % 16, 0u)
        << "Alignment should be a multiple of 16";
}
