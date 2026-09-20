#include "OloEnginePCH.h"
#include "../TestOptions.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/ThreadLocalCache.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

namespace
{
    // steady_clock, not high_resolution_clock: the latter is an alias of
    // system_clock in libstdc++, so a wall-clock adjustment mid-run would
    // change the measured duration on Linux CI.
    using AllocBenchClock = std::chrono::steady_clock;

    /// Whether --olo-bench-assert was passed.
    bool AllocatorBenchAssertEnabled()
    {
        return OloEngine::Tests::Options().BenchAssert;
    }
} // namespace

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
    EXPECT_EQ(data->materialDataIndex, static_cast<u16>(60));
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

// ConstantsAreSensible: all three checks are compile-time facts. Replaced
// with static_asserts (docs/testing.md section 4.3 -- static
// assert in disguise). If any of these regress, the build fails before
// the test runner starts.
static_assert(CommandAllocator::DEFAULT_BLOCK_SIZE >= 4096u,
              "DEFAULT_BLOCK_SIZE must be at least 4 KB");
static_assert(CommandAllocator::MAX_COMMAND_SIZE >= sizeof(DrawMeshCommand),
              "MAX_COMMAND_SIZE must fit the largest command");
static_assert(CommandAllocator::COMMAND_ALIGNMENT % 16 == 0u,
              "COMMAND_ALIGNMENT must be a multiple of 16");
// MAX_COMMAND_SIZE is declared TWICE -- CommandAllocator::MAX_COMMAND_SIZE
// (the allocator's own reject threshold) and OloEngine::MAX_COMMAND_SIZE in
// Commands/RenderCommand.h (what CommandPacket's per-command static_assert
// binds to). Neither header includes the other, so nothing in the engine ever
// sees both and the two can drift apart silently: raise only the namespace one
// and every command still compiles while the allocator refuses to allocate it
// at runtime. This TU includes both headers, which makes it the only place the
// pairing can be checked at all.
static_assert(CommandAllocator::MAX_COMMAND_SIZE == OloEngine::MAX_COMMAND_SIZE,
              "The two MAX_COMMAND_SIZE declarations drifted -- update both "
              "Commands/CommandAllocator.h and Commands/RenderCommand.h");

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

// =============================================================================
// Invariants across block rollover (issue #1328)
//
// The pre-fix Allocate() sized a rollover block using padding computed for the
// OLD block's base address, then recomputed padding against the NEW base with
// no final bounds check. The two paddings only differ when the two bases have
// different alignment residues, so a test that rolls over once, from one cache,
// at one address passes against the broken code. Every test below that
// exercises rollover therefore sweeps several freshly constructed caches.
// =============================================================================

// A huge-but-representable allocation request is a clean nullptr from the
// cache, but a sanitizer's allocator intercepts it first and aborts the whole
// process: ASan and TSan both treat an over-large request as a fatal
// ReportAllocationSizeTooBig unless allocator_may_return_null=1, which the CI
// jobs do not set. So that one sub-case is skipped under ANY sanitizer -- not
// just ASan -- rather than weakening the check everywhere. The plain Debug and
// Release runs still cover it.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define OLO_TEST_UNDER_SANITIZER 1
#endif
#endif
#if !defined(OLO_TEST_UNDER_SANITIZER)
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define OLO_TEST_UNDER_SANITIZER 1
#else
#define OLO_TEST_UNDER_SANITIZER 0
#endif
#endif

namespace
{
    // Alignments at or above COMMAND_ALIGNMENT, up to the documented maximum.
    // The upper end is "over-aligned" relative to a block's natural alignment.
    constexpr std::array<sizet, 5> kOverAlignments{ 16, 32, 64, 128, 256 };

    // An allocation must lie wholly inside the block the cache is bumping, be
    // aligned as asked, and leave the cursor at or before the block end.
    ::testing::AssertionResult AllocationIsSound(const ThreadLocalCache& cache, const void* p, sizet size, sizet alignment)
    {
        const MemoryBlock* block = cache.GetCurrentBlock();
        if (!block || !block->Data)
        {
            return ::testing::AssertionFailure() << "cache has no current block";
        }

        auto const addr = reinterpret_cast<std::uintptr_t>(p);
        auto const base = reinterpret_cast<std::uintptr_t>(block->Data);

        if (addr % alignment != 0)
        {
            return ::testing::AssertionFailure() << "address 0x" << std::hex << addr << std::dec << " is not " << alignment << "-byte aligned";
        }
        if (addr < base || (addr - base) > block->Size || size > block->Size - (addr - base))
        {
            return ::testing::AssertionFailure()
                   << "allocation at block offset " << (addr < base ? 0 : addr - base) << " for " << size << " bytes escapes its " << block->Size
                   << "-byte block";
        }
        if (block->Offset > block->Size)
        {
            return ::testing::AssertionFailure() << "cursor " << block->Offset << " is past the block end " << block->Size;
        }
        return ::testing::AssertionSuccess();
    }

    // The whole returned range must be writable. Under ASan an out-of-bounds
    // allocation reports here instead of silently corrupting a neighbour.
    void TouchEveryByte(void* p, sizet size)
    {
        std::memset(p, 0xAB, size);
    }
} // namespace

TEST(ThreadLocalCache, RolloverStaysInsideTheNewBlockAcrossBaseAddresses)
{
    constexpr sizet kBlockSize = 192;
    // Larger than the default block, so the *request* drives the new block's
    // size. That is the case where padding the old code failed to reserve is
    // unreserved rather than absorbed by the default size.
    constexpr sizet kRequest = kBlockSize + 32;

    for (sizet alignment : kOverAlignments)
    {
        for (int trial = 0; trial < 16; ++trial)
        {
            ThreadLocalCache cache(kBlockSize);

            // Bump the cursor off zero so the old base's alignment residue
            // differs from the new block's. Without this the two paddings
            // agree and the bug hides.
            ASSERT_NE(cache.Allocate(24, 8), nullptr);

            void* p = cache.Allocate(kRequest, alignment);
            ASSERT_NE(p, nullptr) << "alignment " << alignment << ", trial " << trial;
            ASSERT_TRUE(AllocationIsSound(cache, p, kRequest, alignment)) << "alignment " << alignment << ", trial " << trial;
            TouchEveryByte(p, kRequest);
        }
    }
}

TEST(ThreadLocalCache, RolloverAfterResetStaysInsideTheReusedBlock)
{
    // Rollover into a *rewound* block is a different path from first-fill
    // rollover: AddBlock serves m_CurrentBlock->Next instead of allocating.
    constexpr sizet kBlockSize = 256;
    constexpr sizet kRequest = 200;

    for (sizet alignment : kOverAlignments)
    {
        for (int trial = 0; trial < 8; ++trial)
        {
            ThreadLocalCache cache(kBlockSize);

            for (int i = 0; i < 3; ++i)
            {
                ASSERT_NE(cache.Allocate(kRequest, 8), nullptr);
            }
            u32 const blocksBefore = cache.GetBlockCount();
            ASSERT_EQ(blocksBefore, 3u);

            cache.Reset();

            for (int i = 0; i < 3; ++i)
            {
                void* p = cache.Allocate(kRequest, alignment);
                ASSERT_NE(p, nullptr) << "alignment " << alignment << ", block " << i << ", trial " << trial;
                ASSERT_TRUE(AllocationIsSound(cache, p, kRequest, alignment))
                    << "alignment " << alignment << ", block " << i << ", trial " << trial;
                TouchEveryByte(p, kRequest);
            }

            EXPECT_EQ(cache.GetBlockCount(), blocksBefore)
                << "Reset + re-allocate at alignment " << alignment
                << " grew the chain: the rewound block could not hold what an identically sized fresh block held";
        }
    }
}

TEST(ThreadLocalCache, SizeAndAlignmentBoundaryMatrix)
{
    constexpr std::array<sizet, 15> kSizes{ 1, 7, 8, 15, 16, 17, 63, 64, 65, 127, 128, 129, 255, 256, 257 };
    constexpr std::array<sizet, 9> kAlignments{ 1, 2, 4, 8, 16, 32, 64, 128, 256 };
    // Sized so that some (size, alignment) pairs fit several times over and
    // others roll over on the second or third request.
    constexpr sizet kBlockSize = 320;

    for (sizet alignment : kAlignments)
    {
        for (sizet size : kSizes)
        {
            ThreadLocalCache cache(kBlockSize);
            std::vector<std::pair<std::uintptr_t, sizet>> live;

            for (int i = 0; i < 6; ++i)
            {
                void* p = cache.Allocate(size, alignment);
                ASSERT_NE(p, nullptr) << "size " << size << ", alignment " << alignment << ", iteration " << i;
                ASSERT_TRUE(AllocationIsSound(cache, p, size, alignment)) << "size " << size << ", alignment " << alignment << ", iteration " << i;
                TouchEveryByte(p, size);

                auto const addr = reinterpret_cast<std::uintptr_t>(p);
                for (auto const& [prevAddr, prevSize] : live)
                {
                    EXPECT_TRUE(addr + size <= prevAddr || prevAddr + prevSize <= addr)
                        << "allocation overlaps a live earlier one (size " << size << ", alignment " << alignment << ", iteration " << i << ")";
                }
                live.emplace_back(addr, size);
            }
        }
    }
}

TEST(ThreadLocalCache, RejectsUnsupportedAlignmentWithoutAdvancingTheCursor)
{
    ThreadLocalCache cache(512);

    ASSERT_NE(cache.Allocate(32, 16), nullptr);
    ASSERT_NE(cache.GetCurrentBlock(), nullptr);
    sizet const offsetBefore = cache.GetCurrentBlock()->Offset;
    sizet const allocatedBefore = cache.GetTotalAllocated();
    u32 const blocksBefore = cache.GetBlockCount();

    std::array<sizet, 8> const badAlignments{ 0, 3, 6, 12, 24, 100, ThreadLocalCache::MAX_ALIGNMENT * 2, ~sizet(0) };
    for (sizet alignment : badAlignments)
    {
        EXPECT_EQ(cache.Allocate(32, alignment), nullptr) << "alignment " << alignment << " is neither a power of two nor within the maximum";
        EXPECT_EQ(cache.GetCurrentBlock()->Offset, offsetBefore) << "a rejected allocation moved the cursor (alignment " << alignment << ")";
        EXPECT_EQ(cache.GetTotalAllocated(), allocatedBefore);
        EXPECT_EQ(cache.GetBlockCount(), blocksBefore);
    }

    // Every supported alignment is still accepted, and the cache still works.
    for (sizet alignment : kOverAlignments)
    {
        void* p = cache.Allocate(32, alignment);
        ASSERT_NE(p, nullptr) << "alignment " << alignment << " must be supported";
        ASSERT_TRUE(AllocationIsSound(cache, p, 32, alignment));
    }
}

TEST(ThreadLocalCache, ZeroSizeIsRejectedWithoutAdvancingTheCursor)
{
    ThreadLocalCache cache(512);

    ASSERT_NE(cache.Allocate(32, 16), nullptr);
    sizet const offsetBefore = cache.GetCurrentBlock()->Offset;
    sizet const allocatedBefore = cache.GetTotalAllocated();

    EXPECT_EQ(cache.Allocate(0, 16), nullptr);
    EXPECT_EQ(cache.Allocate(0, 1), nullptr);
    EXPECT_EQ(cache.GetCurrentBlock()->Offset, offsetBefore);
    EXPECT_EQ(cache.GetTotalAllocated(), allocatedBefore);
}

TEST(ThreadLocalCache, ArithmeticOverflowIsRejectedWithoutAdvancingTheCursor)
{
    ThreadLocalCache cache(512);

    ASSERT_NE(cache.Allocate(32, 16), nullptr);
    sizet const offsetBefore = cache.GetCurrentBlock()->Offset;
    sizet const allocatedBefore = cache.GetTotalAllocated();

    constexpr sizet kMax = ~sizet(0);
    // size + (alignment - 1) wraps for each of these: unchecked, the wrapped
    // sum reads as a tiny request and the cursor walks out of the block.
    std::array<std::pair<sizet, sizet>, 4> const overflowing{ { { kMax, 16 }, { kMax - 8, 16 }, { kMax, 256 }, { kMax - 254, 256 } } };
    for (auto const& [size, alignment] : overflowing)
    {
        EXPECT_EQ(cache.Allocate(size, alignment), nullptr) << "size " << size << " at alignment " << alignment << " overflows and must be rejected";
        EXPECT_EQ(cache.GetCurrentBlock()->Offset, offsetBefore);
        EXPECT_EQ(cache.GetTotalAllocated(), allocatedBefore);
    }

#if !OLO_TEST_UNDER_SANITIZER
    // Representable, but no heap can back it: still a clean nullptr, not a
    // throw and not a wrapped cursor.
    EXPECT_EQ(cache.Allocate(kMax / 2, 16), nullptr);
    EXPECT_EQ(cache.GetCurrentBlock()->Offset, offsetBefore);
    EXPECT_EQ(cache.GetTotalAllocated(), allocatedBefore);
#endif

    void* p = cache.Allocate(64, 16);
    ASSERT_NE(p, nullptr) << "the cache must remain usable after every rejection";
    ASSERT_TRUE(AllocationIsSound(cache, p, 64, 16));
}

TEST(ThreadLocalCache, ReuseChainHandsBackTheSameAddresses)
{
    // Bulk reset is a load-bearing property of this design: Reset() must rewind
    // to byte-identical addresses, not merely to "some" valid memory.
    ThreadLocalCache cache(512);

    std::vector<void*> firstRound;
    for (int i = 0; i < 8; ++i)
    {
        void* p = cache.Allocate(48, 16);
        ASSERT_NE(p, nullptr) << "allocation " << i;
        firstRound.push_back(p);
    }
    u32 const blocksAfterFirstRound = cache.GetBlockCount();

    for (int cycle = 0; cycle < 4; ++cycle)
    {
        cache.Reset();
        EXPECT_EQ(cache.GetTotalAllocated(), 0u);

        for (int i = 0; i < 8; ++i)
        {
            void* p = cache.Allocate(48, 16);
            ASSERT_NE(p, nullptr);
            EXPECT_EQ(p, firstRound[static_cast<sizet>(i)]) << "cycle " << cycle << ", allocation " << i << ": Reset must hand back identical addresses";
        }
        EXPECT_EQ(cache.GetBlockCount(), blocksAfterFirstRound) << "cycle " << cycle << " grew the block chain";
    }
}

TEST(ThreadLocalCache, MixedSizeAndAlignmentStressKeepsEveryInvariant)
{
    // Long deterministic sequence across many rollovers and several Reset
    // cycles. Seeded, so a failure here is reproducible.
    // Direct transforms over rng(), not std::uniform_int_distribution: the
    // distributions are not specified to produce the same sequence across
    // standard libraries, so the same seed would pick different inputs on
    // Linux CI than here and "reproducible" would be a claim about one box.
    // See docs/agent-rules/std-distributions-are-not-portable.md.
    ThreadLocalCache cache(1024);
    std::mt19937 rng(1328);

    for (int cycle = 0; cycle < 4; ++cycle)
    {
        for (int i = 0; i < 2000; ++i)
        {
            sizet const size = 1 + (rng() % 600);            // 1 .. 600
            sizet const alignment = sizet(1) << (rng() % 9); // 1 .. 256

            void* p = cache.Allocate(size, alignment);
            ASSERT_NE(p, nullptr) << "cycle " << cycle << ", iteration " << i << ", size " << size << ", alignment " << alignment;
            ASSERT_TRUE(AllocationIsSound(cache, p, size, alignment))
                << "cycle " << cycle << ", iteration " << i << ", size " << size << ", alignment " << alignment;
            TouchEveryByte(p, size);
        }
        cache.Reset();
    }
}

// =============================================================================
// CommandAllocator — both construction APIs reject at the same boundary
// =============================================================================

namespace
{
    struct FitsPayload
    {
        u8 Bytes[64];
    };
    struct ExactlyAtCapPayload
    {
        u8 Bytes[CommandAllocator::MAX_COMMAND_PAYLOAD_SIZE];
    };
    struct OneByteOverCapPayload
    {
        u8 Bytes[CommandAllocator::MAX_COMMAND_PAYLOAD_SIZE + 1];
    };
    struct alignas(32) OverAlignedPayload
    {
        u8 Bytes[64];
    };
} // namespace

// The boundary both construction APIs must reject at is a compile-time one, so
// it cannot be exercised at run time: instantiating either template with a
// rejected payload is a build error by design. What is checked here is the
// shared predicate the two templates now both use -- the defect was that
// AllocatePacketWithCommand carried no size bound at all, so a payload
// CreateCommandPacket refused at compile time got through it and was refused at
// run time instead, as a null packet at draw time.
static_assert(CommandAllocator::PayloadFitsMaxCommandSize<FitsPayload>());
static_assert(CommandAllocator::PayloadFitsMaxCommandSize<ExactlyAtCapPayload>(),
              "A payload exactly at MAX_COMMAND_SIZE minus the packet header must be allowed");
static_assert(!CommandAllocator::PayloadFitsMaxCommandSize<OneByteOverCapPayload>(),
              "The size bound must sit at MAX_COMMAND_SIZE minus sizeof(CommandPacket): the packet and its payload are ONE allocation");
static_assert(CommandAllocator::PayloadPlacementIsAligned<FitsPayload>());
static_assert(!CommandAllocator::PayloadPlacementIsAligned<OverAlignedPayload>(),
              "alignof(T) above COMMAND_ALIGNMENT cannot be honoured for a payload placed at allocationBase + sizeof(CommandPacket)");
// The `||` this replaced accepted any alignof(T) <= COMMAND_ALIGNMENT even when
// the header offset did not preserve it -- and sizeof(CommandPacket) is NOT a
// multiple of COMMAND_ALIGNMENT, so that branch was reachable. Pin the header
// offset: if it ever stops preserving 8-byte alignment, this fails here rather
// than as a misaligned load in a command payload.
static_assert(sizeof(CommandPacket) % 8 == 0,
              "sizeof(CommandPacket) must preserve at least 8-byte payload alignment; "
              "to support a more strongly aligned payload, pad the header up to a multiple of that alignment");

TEST(CommandAllocator, PayloadAtTheCapAllocatesAndOneByteOverDoesNot)
{
    CommandAllocator allocator;

    // Exactly the largest payload the compile-time predicate accepts must also
    // pass the run-time bound in AllocateCommandMemory. If these two disagree,
    // one of them is at the wrong boundary.
    EXPECT_NE(allocator.AllocateCommandMemory(sizeof(CommandPacket) + sizeof(ExactlyAtCapPayload)), nullptr);
    EXPECT_EQ(allocator.AllocateCommandMemory(sizeof(CommandPacket) + sizeof(OneByteOverCapPayload)), nullptr);
}

TEST(CommandAllocator, OversizedRequestIsRejectedAndLeavesTheAllocatorUsable)
{
    CommandAllocator allocator;

    ASSERT_NE(allocator.AllocateCommandMemory(64), nullptr);
    sizet const countBefore = allocator.GetAllocationCount();
    sizet const allocatedBefore = allocator.GetTotalAllocated();

    EXPECT_EQ(allocator.AllocateCommandMemory(CommandAllocator::MAX_COMMAND_SIZE + 1), nullptr);
    EXPECT_EQ(allocator.AllocateCommandMemory(~sizet(0)), nullptr);
    EXPECT_EQ(allocator.GetAllocationCount(), countBefore) << "a rejected request was counted as an allocation";
    EXPECT_EQ(allocator.GetTotalAllocated(), allocatedBefore) << "a rejected request advanced the allocator";

    void* p = allocator.AllocateCommandMemory(CommandAllocator::MAX_COMMAND_SIZE);
    ASSERT_NE(p, nullptr) << "the cap itself must remain allocatable";
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % CommandAllocator::COMMAND_ALIGNMENT, 0u);
}

TEST(CommandAllocator, BothConstructionApisAlignThePayloadAcrossRollover)
{
    // Small blocks so the sequence rolls over many times; every packet from
    // either API must land with its header at COMMAND_ALIGNMENT and its payload
    // at the alignment the static_assert promised.
    CommandAllocator allocator(512);

    for (int i = 0; i < 200; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 2, 1.0f, i);
        CommandPacket* memcpyPacket = allocator.CreateCommandPacket(cmd);
        ASSERT_NE(memcpyPacket, nullptr) << "memcpy construction failed at " << i;
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(memcpyPacket) % CommandAllocator::COMMAND_ALIGNMENT, 0u);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(memcpyPacket->GetRawCommandData()) % alignof(DrawMeshCommand), 0u)
            << "memcpy-constructed payload is misaligned at " << i;
        EXPECT_EQ(memcpyPacket->GetCommandData<DrawMeshCommand>()->entityID, i);

        CommandPacket* placementPacket = allocator.AllocatePacketWithCommand<SetViewportCommand>();
        ASSERT_NE(placementPacket, nullptr) << "placement construction failed at " << i;
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(placementPacket) % CommandAllocator::COMMAND_ALIGNMENT, 0u);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(placementPacket->GetRawCommandData()) % alignof(SetViewportCommand), 0u)
            << "placement-constructed payload is misaligned at " << i;
    }
}

// =============================================================================
// Hot path — 16-byte command cost and allocations per frame
// =============================================================================

TEST(CommandAllocator, HotPathCostAndAllocationsPerFrameDoNotRegress)
{
    // The structural half is deterministic and always asserted: one allocation
    // per command, an exact byte total, and no growth across frames. That is
    // what "allocations per frame do not regress" means for a bump allocator.
    // The timing half is printed always and asserted only under
    // --olo-bench-assert, because wall clock on this box is not a CI signal.
    constexpr sizet kCommandsPerFrame = 2000;
    constexpr sizet kFrames = 50;
    constexpr sizet kCommandBytes = 16; // the normal 16-byte command path

    CommandAllocator allocator;

    // Nothing inside the timed region may run GoogleTest's comparison
    // machinery: at 2000 x 50 allocations its cost lands in ns/allocation and
    // the printed figure stops being about the allocator. Record per frame,
    // assert afterwards.
    std::vector<sizet> allocationCounts(kFrames, 0);
    std::vector<sizet> byteTotals(kFrames, 0);
    sizet nullResults = 0;
    sizet misalignedResults = 0;

    auto const start = AllocBenchClock::now();
    for (sizet frame = 0; frame < kFrames; ++frame)
    {
        allocator.Reset();
        for (sizet i = 0; i < kCommandsPerFrame; ++i)
        {
            void* p = allocator.AllocateCommandMemory(kCommandBytes);
            nullResults += (p == nullptr) ? 1 : 0;
            misalignedResults += (reinterpret_cast<std::uintptr_t>(p) % CommandAllocator::COMMAND_ALIGNMENT) != 0 ? 1 : 0;
        }
        allocationCounts[frame] = allocator.GetAllocationCount();
        byteTotals[frame] = allocator.GetTotalAllocated();
    }
    auto const elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(AllocBenchClock::now() - start).count();

    ASSERT_EQ(nullResults, 0u) << nullResults << " allocations returned null";
    ASSERT_EQ(misalignedResults, 0u) << misalignedResults << " allocations were not " << CommandAllocator::COMMAND_ALIGNMENT << "-byte aligned";
    for (sizet frame = 0; frame < kFrames; ++frame)
    {
        ASSERT_EQ(allocationCounts[frame], kCommandsPerFrame) << "frame " << frame << " did not allocate exactly once per command";
        ASSERT_EQ(byteTotals[frame], kCommandsPerFrame * kCommandBytes)
            << "frame " << frame << " did not cost exactly " << kCommandBytes << " bytes per command";
    }

    double const nsPerAllocation = static_cast<double>(elapsedNs) / static_cast<double>(kFrames * kCommandsPerFrame);
    std::cout << "[ BENCH    ] CommandAllocator 16-byte path: " << nsPerAllocation << " ns/allocation over " << (kFrames * kCommandsPerFrame)
              << " allocations\n";

    if (AllocatorBenchAssertEnabled())
    {
        // Generous upper bound -- a correctness guard against an accidental
        // per-allocation syscall or heap hit, not a perf target.
        EXPECT_LT(nsPerAllocation, 500.0) << "the 16-byte hot path regressed badly";
    }
}
