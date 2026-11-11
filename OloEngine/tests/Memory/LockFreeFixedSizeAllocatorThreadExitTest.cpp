// OLO_TEST_LAYER: unit
//
// TLockFreeFixedSizeAllocator_TLSCache keeps each thread's freed blocks in a
// thread-local cache of up to two bundles. A thread that exits must hand them
// back: the scheduler's workers are such threads, and WorkerRestartTest, which
// restarts them a thousand times, grew past 6 GB on a CPU-starved CI runner
// when every exiting worker stranded its cache and Allocate replaced it with
// fresh bundles.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/LockFreeFixedSizeAllocator.h"
#include "OloEngine/Memory/NoopCounter.h"

#include <thread>
#include <vector>

namespace
{
    using namespace OloEngine;

    // A block size nothing else in the process uses, so this allocator type's
    // thread caches and free lists hold only this test's blocks.
    constexpr i32 kBlockSize = 4104;
    constexpr i32 kPerBundle = 65536 / kBlockSize; // 15
    using FAllocator = TLockFreeFixedSizeAllocator_TLSCache<kBlockSize, OLO_PLATFORM_CACHE_LINE_SIZE, FAtomicCounter>;

    // Never destroyed, as the bundles it hands out are never freed: destroying
    // it empties its free list and leaves the listed bundles unreachable, which
    // LeakSanitizer reports.
    FAllocator& Allocator()
    {
        static FAllocator* s_Allocator = new FAllocator();
        return *s_Allocator;
    }

    // Blocks the allocator has ever carved from a fresh bundle.
    [[nodiscard]] i32 Capacity()
    {
        return Allocator().GetNumUsed().GetValue() + Allocator().GetNumFree().GetValue();
    }

    template<typename Body>
    void OnAThreadThatThenExits(Body body)
    {
        std::thread thread(body);
        thread.join();
    }

    struct FFreesItsBlockWhenDestroyed
    {
        void* Block = nullptr;

        ~FFreesItsBlockWhenDestroyed()
        {
            Allocator().Free(Block);
        }
    };
} // namespace

TEST(LockFreeFixedSizeAllocatorThreadExit, AnExitedThreadsCachedBlocksAreReusedNotReplaced)
{
    constexpr i32 kTaken = 3 * kPerBundle;
    constexpr i32 kHandedOn = 8;
    std::vector<void*> handedOn;

    // Frees all but 8 of three bundles' blocks: one bundle reaches the global
    // list, and the thread exits caching a full bundle and 7 blocks.
    OnAThreadThatThenExits(
        [&handedOn]
        {
            std::vector<void*> blocks;
            for (i32 i = 0; i < kTaken; ++i)
            {
                blocks.push_back(Allocator().Allocate());
            }
            for (i32 i = 0; i < kTaken - kHandedOn; ++i)
            {
                Allocator().Free(blocks[static_cast<sizet>(i)]);
            }
            handedOn.assign(blocks.end() - kHandedOn, blocks.end());
        });

    // Frees the other 8 and exits with them cached: with the first thread's 7,
    // a whole bundle's worth.
    OnAThreadThatThenExits(
        [&handedOn]
        {
            for (void* block : handedOn)
            {
                Allocator().Free(block);
            }
        });

    // Every block is free again, so a new thread can take all three bundles'
    // worth without the allocator carving a fresh bundle.
    const i32 before = Capacity();
    OnAThreadThatThenExits(
        []
        {
            std::vector<void*> blocks;
            for (i32 i = 0; i < kTaken; ++i)
            {
                blocks.push_back(Allocator().Allocate());
            }
            for (void* block : blocks)
            {
                Allocator().Free(block);
            }
        });
    EXPECT_EQ(Capacity(), before) << (Capacity() - before) / kPerBundle
                                  << " fresh bundles were carved while exited threads' cached blocks were stranded";
}

// thread_local destructors run in reverse order of construction, so one that
// was constructed before the thread first used the allocator runs after the
// thread's cache was handed back. A block it frees then must reach the next
// thread too, not stay in the cache of a thread that is gone.
TEST(LockFreeFixedSizeAllocatorThreadExit, ABlockFreedAfterTheCacheWasHandedBackIsNotStranded)
{
    // Enough threads to strand more blocks than the earlier test can have left free.
    constexpr i32 kThreads = 8 * kPerBundle;

    const i32 before = Capacity();
    for (i32 t = 0; t < kThreads; ++t)
    {
        OnAThreadThatThenExits(
            []
            {
                thread_local FFreesItsBlockWhenDestroyed freesItsBlockWhenDestroyed;
                freesItsBlockWhenDestroyed.Block = Allocator().Allocate();
            });
    }

    // One block is in use at a time, so at most one bundle is ever carved.
    EXPECT_LE(Capacity() - before, kPerBundle)
        << (Capacity() - before) << " blocks carved by " << kThreads << " threads that each used one block";
}
