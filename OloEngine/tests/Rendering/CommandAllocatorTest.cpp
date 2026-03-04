#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/ThreadLocalCache.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <vector>
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
    for (sizet size : { 1, 7, 16, 33, 64, 128, 255, 512, 1024 })
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

// =============================================================================
// ThreadLocalCache — Block Reuse After Reset
// =============================================================================

TEST(ThreadLocalCache, SingleBlockForSmallAllocations)
{
    ThreadLocalCache cache(256); // Small blocks for testing

    // Allocate less than one block — should stay at 1 block
    cache.Allocate(64);
    cache.Allocate(64);
    EXPECT_EQ(cache.GetBlockCount(), 1u);
}

TEST(ThreadLocalCache, MultiBlockAllocationGrows)
{
    ThreadLocalCache cache(128); // Tiny blocks to force multiple blocks

    // Fill the first block
    cache.Allocate(100);
    EXPECT_EQ(cache.GetBlockCount(), 1u);

    // This should spill into a second block
    cache.Allocate(100);
    EXPECT_EQ(cache.GetBlockCount(), 2u);

    // And a third
    cache.Allocate(100);
    EXPECT_EQ(cache.GetBlockCount(), 3u);
}

TEST(ThreadLocalCache, ResetReusesBlocksWithoutLeaking)
{
    // BUG REGRESSION: AddBlock used to overwrite m_CurrentBlock->Next,
    // orphaning existing blocks in the chain after Reset().
    ThreadLocalCache cache(128);

    // Fill 3 blocks
    cache.Allocate(100);
    cache.Allocate(100);
    cache.Allocate(100);
    EXPECT_EQ(cache.GetBlockCount(), 3u) << "Should have 3 blocks after initial allocations";

    // Reset — rewinds to first block, all offsets zeroed
    cache.Reset();
    EXPECT_EQ(cache.GetBlockCount(), 3u) << "Reset must not free blocks";

    // Re-allocate the same pattern — must REUSE existing blocks, not create new ones
    cache.Allocate(100);
    cache.Allocate(100);
    cache.Allocate(100);
    EXPECT_EQ(cache.GetBlockCount(), 3u)
        << "Block count should remain 3 after Reset + re-allocation.\n"
        << "If it grew, AddBlock is leaking blocks by overwriting Next pointers.";

    // Do it again to verify stability across multiple Reset cycles
    cache.Reset();
    cache.Allocate(100);
    cache.Allocate(100);
    cache.Allocate(100);
    EXPECT_EQ(cache.GetBlockCount(), 3u)
        << "Block count must remain stable across multiple Reset cycles";
}

TEST(ThreadLocalCache, ResetAndReallocateSameMemoryFootprint)
{
    // Verify that reuse after Reset doesn't grow memory footprint.
    ThreadLocalCache cache(256);

    // First round: allocate enough to span 2 blocks
    for (int i = 0; i < 4; ++i)
        cache.Allocate(128);
    u32 blockCountAfterFirstRound = cache.GetBlockCount();
    EXPECT_GE(blockCountAfterFirstRound, 2u);

    // Multiple Reset + re-allocate cycles
    for (int cycle = 0; cycle < 5; ++cycle)
    {
        cache.Reset();
        for (int i = 0; i < 4; ++i)
            cache.Allocate(128);
        EXPECT_EQ(cache.GetBlockCount(), blockCountAfterFirstRound)
            << "Block count grew on cycle " << cycle << " — memory leak!";
    }
}

TEST(ThreadLocalCache, OversizedAllocationGetsLargerBlock)
{
    ThreadLocalCache cache(128);

    // Request something larger than the default block size
    void* mem = cache.Allocate(256);
    ASSERT_NE(mem, nullptr);

    // Should have 2 blocks: the initial 128-byte block + one 256-byte block
    EXPECT_EQ(cache.GetBlockCount(), 2u);
}

TEST(ThreadLocalCache, AllocateReturnsAlignedPointers)
{
    ThreadLocalCache cache(512);

    for (sizet alignment : { 8, 16, 32 })
    {
        void* mem = cache.Allocate(64, alignment);
        ASSERT_NE(mem, nullptr);
        auto addr = reinterpret_cast<std::uintptr_t>(mem);
        EXPECT_EQ(addr % alignment, 0u)
            << "Allocation not aligned to " << alignment << " bytes";
    }
}

// =============================================================================
// CommandAllocator — Reuse Stability Across Reset
// =============================================================================

TEST(CommandAllocator, ThreadCacheIsReusedAfterReset)
{
    // Verify that the allocator reuses its memory blocks across Reset() calls,
    // rather than growing memory every frame.
    CommandAllocator allocator;

    // First allocation creates a thread cache
    allocator.AllocateCommandMemory(64);
    sizet allocated1 = allocator.GetTotalAllocated();

    // Reset and allocate again — should reuse the same thread cache
    allocator.Reset();
    allocator.AllocateCommandMemory(64);
    sizet allocated2 = allocator.GetTotalAllocated();

    // Total allocated should be roughly the same (blocks reused)
    EXPECT_LE(allocated2, allocated1)
        << "Allocator blocks were not reused after Reset";
}

TEST(CommandAllocator, MultiBlockResetDoesNotLeak)
{
    // Test the multi-block scenario through the CommandAllocator interface
    CommandAllocator allocator(256); // Small blocks to force multi-block

    // Fill multiple blocks
    for (int i = 0; i < 10; ++i)
        allocator.AllocateCommandMemory(128);

    sizet allocated1 = allocator.GetTotalAllocated();

    // Reset and re-allocate — should reuse blocks
    allocator.Reset();
    for (int i = 0; i < 10; ++i)
        allocator.AllocateCommandMemory(128);

    sizet allocated2 = allocator.GetTotalAllocated();

    // Memory footprint should not grow (blocks are reused)
    EXPECT_LE(allocated2, allocated1)
        << "Memory grew after Reset — AddBlock may be leaking blocks";
}
